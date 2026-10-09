#include <Aster/Assets/AssetImporter.h>
#include <Aster/Renderer/Environment.h>
#include <Aster/Renderer/Renderer.h>
#include <Aster/Scene/Scene.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void SaveImage(const Aster::RenderImage& image, const std::filesystem::path& path)
	{
		std::ofstream stream(path, std::ios::binary);
		stream << "P6\n" << image.Width << " " << image.Height << "\n255\n";
		for (size_t index = 0; index < image.Pixels.size(); index += 4)
		{
			stream.write(reinterpret_cast<const char*>(image.Pixels.data() + index), 3);
		}
		Require(stream.good(), "Cannot write corpus image: " + path.string());
	}

	size_t ChangedPixels(const Aster::RenderImage& first, const Aster::RenderImage& second)
	{
		Require(first.Width == second.Width && first.Height == second.Height &&
					first.Pixels.size() == second.Pixels.size(),
				"Corpus readback dimensions changed unexpectedly");
		size_t changed = 0;
		for (size_t index = 0; index < first.Pixels.size(); index += 4)
		{
			int difference = 0;
			for (size_t channel = 0; channel < 3; ++channel)
			{
				difference +=
					std::abs(static_cast<int>(first.Pixels[index + channel]) - second.Pixels[index + channel]);
			}
			changed += difference > 12 ? 1 : 0;
		}
		return changed;
	}

	struct ExpectedAsset
	{
		const char* Name;
		size_t Primitives;
		size_t Materials;
		size_t Textures;
		size_t Vertices;
	};

	void Run(const std::filesystem::path& assets, const std::filesystem::path& evidence)
	{
		// Counts are from the independently authored, pinned upstream GLB documents.
		const std::array expectedAssets{
			ExpectedAsset{"Lantern", 3, 1, 4, 4145}, ExpectedAsset{"BoomBox", 1, 1, 4, 3575},
			ExpectedAsset{"AlphaBlendModeTest", 9, 6, 4, 137}, ExpectedAsset{"NormalTangentTest", 1, 1, 3, 3983},
			ExpectedAsset{"TextureCoordinateTest", 5, 5, 1, 20}};
		std::filesystem::create_directories(evidence);
		Aster::AssetImporter importer(assets);
		Aster::RendererOptions options;
		options.Width = 320;
		options.Height = 320;
		Aster::Renderer renderer(options);
		std::cout << "Corpus Vulkan device: " << renderer.GetDeviceName() << "\n";
		const Aster::AssetImporter originalAssets(std::filesystem::path(ASTER_SOURCE_DIR) / "Assets");
		renderer.SetEnvironment(
			Aster::EnvironmentProcessor::Build(originalAssets.LoadHDR("Environment/StudioSmall09.hdr")));
		Aster::RenderSettings settings;
		settings.UseSceneEnvironment = false;
		settings.DrawSky = false;
		settings.CameraOverride =
			Aster::RenderCamera{glm::lookAtRH(glm::vec3(0, 0, 3.7f), glm::vec3(0), glm::vec3(0, 1, 0)),
								glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 20.0f)};

		for (const auto& expected : expectedAssets)
		{
			const std::string name = expected.Name;
			const std::string path = name + "/" + name + ".glb";
			const auto mesh = importer.LoadMesh(path);
			Require(mesh.Primitives.size() == expected.Primitives, name + ": lost mesh primitives");
			Require(mesh.Materials.size() == expected.Materials + 1, name + ": lost materials or fallback");
			Require(mesh.Textures.size() == expected.Textures, name + ": lost decoded textures");
			glm::vec3 minimum(std::numeric_limits<float>::max());
			glm::vec3 maximum(std::numeric_limits<float>::lowest());
			size_t vertices = 0;
			for (const auto& primitive : mesh.Primitives)
			{
				Require(!primitive.Indices.empty() && primitive.Indices.size() % 3 == 0, name + ": missing triangles");
				vertices += primitive.Vertices.size();
				for (const auto& vertex : primitive.Vertices)
				{
					const glm::vec3 position(primitive.Transform * glm::vec4(vertex.Position, 1));
					minimum = glm::min(minimum, position);
					maximum = glm::max(maximum, position);
				}
			}
			Require(vertices == expected.Vertices, name + ": lost or duplicated vertices");
			for (const auto& texture : mesh.Textures)
			{
				Require(texture.Image.Width > 0 && texture.Image.Height > 0 &&
							texture.Image.Pixels.size() ==
								static_cast<size_t>(texture.Image.Width) * texture.Image.Height * 4,
						name + ": invalid decoded image dimensions");
			}
			if (name == "AlphaBlendModeTest")
			{
				std::set<Aster::MaterialAlphaMode> modes;
				for (const auto& material : mesh.Materials)
				{
					modes.insert(material.AlphaMode);
				}
				Require(modes.size() == 3, "Upstream alpha modes were not preserved");
			}
			const glm::vec3 extent = maximum - minimum;
			const float scale = 2.0f / std::max({extent.x, extent.y, extent.z});
			Require(std::isfinite(scale) && scale > 0, name + ": invalid imported bounds");
			Aster::Scene scene(name);
			const auto model = scene.CreateEntity(name);
			auto& entity = scene.Get(model);
			entity.MeshRenderer = Aster::MeshRendererComponent{};
			entity.MeshRenderer->Mesh = path;
			entity.Transform.Scale = glm::vec3(scale);
			entity.Transform.Translation = -(minimum + maximum) * (0.5f * scale);
			const auto light = scene.CreateEntity("Key");
			scene.Get(light).Light = Aster::LightComponent{};
			scene.Get(light).Light->Intensity = 2.0f;
			scene.Get(light).Transform.Rotation = {-0.3f, -0.4f, 0};
			renderer.RenderScene(scene, importer, settings);
			const auto visible = renderer.ReadbackRgba8();
			SaveImage(visible, evidence / (name + ".ppm"));
			entity.MeshRenderer->Visible = false;
			renderer.RenderScene(scene, importer, settings);
			const auto hidden = renderer.ReadbackRgba8();
			const auto visiblePixels = ChangedPixels(visible, hidden);
			Require(visiblePixels > 500, name + ": imported geometry did not produce a visible GPU image");
			entity.MeshRenderer->Visible = true;
			const auto pivot = scene.CreateEntity("Rotation pivot");
			scene.SetParent(model, pivot);
			scene.Get(pivot).Transform.Rotation.y = 0.5f;
			renderer.RenderScene(scene, importer, settings);
			const auto rotated = renderer.ReadbackRgba8();
			const auto rotatedPixels = ChangedPixels(visible, rotated);
			Require(rotatedPixels > 250, name + ": parent rotation did not change the rendered model");
			std::cout << name << ": " << vertices << " vertices, " << visiblePixels << " visible pixels, "
					  << rotatedPixels << " rotated pixels\n";
			renderer.InvalidateAssets();
		}
		renderer.Shutdown();
		std::string diagnostics;
		for (const auto& message : renderer.GetValidationMessages())
		{
			diagnostics += message + "\n";
		}
		Require(diagnostics.empty(), "Corpus Vulkan/NVRHI diagnostics:\n" + diagnostics);
	}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		Require(argc == 3, "Usage: AsterAssetCorpusTests <downloaded-corpus> <evidence-directory>");
		Run(argv[1], argv[2]);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Asset corpus: " << error.what() << "\n";
		return 1;
	}
}
