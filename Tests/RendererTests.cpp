#include <Aster/Assets/AssetImporter.h>
#include <Aster/Renderer/Environment.h>
#include <Aster/Renderer/Renderer.h>
#include <Aster/Scene/Scene.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
	void RunSceneRenderingTests(Aster::RendererOptions options);
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	template <typename Exception, typename Callback> void RequireThrows(Callback callback, const char* message)
	{
		try
		{
			callback();
		}
		catch (const Exception&)
		{
			return;
		}
		throw std::runtime_error(message);
	}

	void RequireColor(const Aster::RenderImage& image, uint32_t width, uint32_t height,
					  const std::array<float, 4>& color)
	{
		Require(image.Width == width && image.Height == height,
				"Readback dimensions differ from render target dimensions");
		Require(image.Pixels.size() == static_cast<size_t>(width) * height * 4, "Readback has an incorrect byte count");
		for (size_t index = 0; index < image.Pixels.size(); ++index)
		{
			const int expected = static_cast<int>(std::round(color[index % 4] * 255.0f));
			if (std::abs(static_cast<int>(image.Pixels[index]) - expected) > 1)
			{
				throw std::runtime_error("GPU pixel mismatch at byte " + std::to_string(index) + ": expected " +
										 std::to_string(expected) + ", received " +
										 std::to_string(image.Pixels[index]));
			}
		}
	}

	void RequireClean(const Aster::Renderer& renderer)
	{
		const auto messages = renderer.GetValidationMessages();
		if (!messages.empty())
		{
			std::string diagnostic = "Vulkan/NVRHI validation reported diagnostics:";
			for (const auto& message : messages)
			{
				diagnostic += "\n" + message;
			}
			throw std::runtime_error(diagnostic);
		}
	}
} // namespace

void RunRendererTests()
{
	Aster::RendererOptions invalid;
	invalid.Width = 0;
	RequireThrows<std::invalid_argument>([&]() { Aster::Renderer renderer(invalid); }, "Zero width was accepted");
	invalid.Width = 16385;
	RequireThrows<std::invalid_argument>([&]() { Aster::Renderer renderer(invalid); }, "Excessive width was accepted");
	invalid.Width = 1;
	invalid.Title = std::string("invalid\0title", 13);
	RequireThrows<std::invalid_argument>([&]() { Aster::Renderer renderer(invalid); },
										 "NUL-containing window title was accepted");

	Aster::RendererOptions options;
	options.Width = 37;
	options.Height = 23;
	options.Headless = std::getenv("ASTER_TEST_WINDOW") == nullptr;
	options.Visible = false;
	auto unavailableDevice = options;
	unavailableDevice.DeviceName = "Aster nonexistent GPU used to test initialization cleanup";
	RequireThrows<std::runtime_error>([&]() { Aster::Renderer unavailable(unavailableDevice); },
									  "Unavailable GPU was accepted");
	Aster::Renderer renderer(options);
	RequireThrows<std::logic_error>([&]() { Aster::Renderer duplicate(options); },
									"A second active Vulkan dispatcher was accepted");
	if (!options.Headless)
	{
		auto* window = static_cast<GLFWwindow*>(renderer.GetNativeWindow());
		const auto key = glfwSetKeyCallback(window, nullptr);
		glfwSetKeyCallback(window, key);
		const auto mouse = glfwSetMouseButtonCallback(window, nullptr);
		glfwSetMouseButtonCallback(window, mouse);
		const auto cursor = glfwSetCursorPosCallback(window, nullptr);
		glfwSetCursorPosCallback(window, cursor);
		const auto scroll = glfwSetScrollCallback(window, nullptr);
		glfwSetScrollCallback(window, scroll);
		const auto focus = glfwSetWindowFocusCallback(window, nullptr);
		glfwSetWindowFocusCallback(window, focus);
		Require(key && mouse && cursor && scroll && focus, "Renderer did not install the GLFW input bridge");
		renderer.PollEvents();
		focus(window, GLFW_TRUE);
		key(window, GLFW_KEY_W, 0, GLFW_PRESS, 0);
		key(window, GLFW_KEY_W, 0, GLFW_REPEAT, 0);
		mouse(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
		cursor(window, 5, 7);
		cursor(window, 8, 12);
		scroll(window, 1, -2);
		auto input = renderer.GetInputSnapshot();
		Require(input.KeysDown.contains("W") && input.KeysPressed.contains("W") && input.MouseDown[0] &&
					input.MousePressed[0],
				"GLFW input press/repeat did not reach the snapshot");
		Require(input.MousePosition == glm::vec2(8, 12) && input.Wheel == glm::vec2(1, -2),
				"GLFW mouse/scroll coordinates did not reach the snapshot");
		renderer.PollEvents();
		input = renderer.GetInputSnapshot();
		Require(input.KeysPressed.empty() && !input.MousePressed[0] && input.Wheel == glm::vec2(0),
				"Polling did not clear input edges and scroll");
		focus(window, GLFW_FALSE);
		input = renderer.GetInputSnapshot();
		Require(!input.Focused && input.KeysDown.empty() && input.KeysReleased.contains("W") && !input.MouseDown[0] &&
					input.MouseReleased[0],
				"GLFW focus loss left input stuck down");
	}
	std::cout << "Renderer device: " << renderer.GetDeviceName() << "\n";
	Require(!renderer.GetDeviceName().empty(), "Device name must identify the real Vulkan device");
	RequireThrows<std::logic_error>([&]() { renderer.ReadbackRgba8(); }, "Readback before rendering was accepted");
	RequireThrows<std::invalid_argument>([&]()
										 { renderer.RenderFrame({std::numeric_limits<float>::quiet_NaN(), 0, 0, 1}); },
										 "Nonfinite clear color was accepted");
	RequireThrows<std::invalid_argument>([&]() { renderer.RenderFrame({-0.1f, 0, 0, 1}); },
										 "Out-of-range clear color was accepted");
	RequireThrows<std::invalid_argument>([&]() { renderer.Resize(0, 3); }, "Invalid resize was accepted");

	bool rejectedWrongThread = false;
	std::thread wrongThread(
		[&]()
		{
			try
			{
				renderer.RenderFrame();
			}
			catch (const std::logic_error&)
			{
				rejectedWrongThread = true;
			}
		});
	wrongThread.join();
	Require(rejectedWrongThread, "Renderer accepted calls from a different thread");

	for (const auto& color : {std::array<float, 4>{1, 0, 0.25f, 1}, std::array<float, 4>{0, 1, 0.5f, 0.75f},
							  std::array<float, 4>{0.1f, 0.2f, 1, 0}})
	{
		renderer.PollEvents();
		renderer.RenderFrame(color);
		RequireColor(renderer.ReadbackRgba8(), renderer.GetWidth(), renderer.GetHeight(), color);
		Require(!renderer.ShouldClose(), "Window closed unexpectedly");
	}
	RequireClean(renderer);

	bool overlayCalled = false;
	renderer.SetOverlayCallback(
		[&](nvrhi::IDevice* device, nvrhi::ICommandList* commands, nvrhi::IFramebuffer* target)
		{
			Require(device && commands && target, "Overlay callback did not receive borrowed graphics resources");
			overlayCalled = true;
			RequireThrows<std::logic_error>([&]() { renderer.RenderFrame(); }, "Overlay reentered frame rendering");
			RequireThrows<std::logic_error>([&]() { renderer.Resize(2, 2); },
											"Overlay resized an active render target");
			RequireThrows<std::logic_error>([&]() { renderer.ReadbackRgba8(); }, "Overlay reentered readback");
			RequireThrows<std::logic_error>([&]() { renderer.Shutdown(); }, "Overlay shut down an active renderer");
			renderer.SetOverlayCallback({});
			throw std::runtime_error("Expected overlay failure");
		});
	RequireThrows<std::runtime_error>([&]() { renderer.RenderFrame(); }, "Overlay exception was swallowed");
	Require(overlayCalled, "Overlay callback was not invoked");
	Require((renderer.GetNativeWindow() == nullptr) == options.Headless,
			"Native window availability differs from headless option");
	renderer.RenderFrame();
	RequireClean(renderer);

	renderer.Resize(61, 17);
	Require(renderer.GetWidth() == 61 && renderer.GetHeight() == 17, "Resize did not update render target dimensions");
	RequireThrows<std::logic_error>([&]() { renderer.ReadbackRgba8(); }, "Resize retained stale frame contents");
	const std::array<float, 4> resizedColor{0.5f, 0.125f, 0.75f, 1};
	renderer.RenderFrame(resizedColor);
	RequireColor(renderer.ReadbackRgba8(), 61, 17, resizedColor);
	RequireColor(renderer.ReadbackRgba8(), 61, 17, resizedColor);
	RequireClean(renderer);

	renderer.Shutdown();
	renderer.Shutdown();
	RequireClean(renderer);
	RequireThrows<std::logic_error>([&]() { renderer.RenderFrame(); }, "Renderer accepted work after shutdown");
	RequireThrows<std::logic_error>([&]() { renderer.ReadbackRgba8(); }, "Renderer accepted readback after shutdown");
	Aster::RendererOptions secondOptions = options;
	secondOptions.Width = 11;
	secondOptions.Height = 13;
	Aster::Renderer second(secondOptions);
	second.RenderFrame(resizedColor);
	RequireColor(second.ReadbackRgba8(), 11, 13, resizedColor);
	second.Shutdown();
	RequireClean(second);
	std::cout << "Validated frame readback, resize, thread ownership, and shutdown\n";
	RunSceneRenderingTests(options);
}

namespace
{
	int ToneMap(float radiance, float exposure)
	{
		const float hdr = radiance * exposure;
		const float mapped =
			std::clamp((hdr * (2.51f * hdr + 0.03f)) / (hdr * (2.43f * hdr + 0.59f) + 0.14f), 0.0f, 1.0f);
		const float srgb = mapped <= 0.0031308f ? 12.92f * mapped : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
		return static_cast<int>(std::round(srgb * 255.0f));
	}

	std::array<uint8_t, 3> CenterPixel(const Aster::RenderImage& image)
	{
		const size_t offset = (static_cast<size_t>(image.Height / 2) * image.Width + image.Width / 2) * 4;
		return {image.Pixels[offset], image.Pixels[offset + 1], image.Pixels[offset + 2]};
	}

	void RequireOcclusionMoved(const Aster::RenderImage& oldLit, const Aster::RenderImage& oldOccluded,
							   const Aster::RenderImage& newLit, const Aster::RenderImage& newOccluded, int threshold)
	{
		Require(oldLit.Pixels.size() == oldOccluded.Pixels.size() && oldLit.Pixels.size() == newLit.Pixels.size() &&
					oldLit.Pixels.size() == newOccluded.Pixels.size(),
				"Motion image dimensions changed unexpectedly");
		size_t released = 0;
		size_t newlyOccluded = 0;
		for (size_t byte = 0; byte < oldLit.Pixels.size(); byte += 4)
		{
			const bool oldMask = int(oldLit.Pixels[byte]) - int(oldOccluded.Pixels[byte]) > threshold;
			const bool newMask = int(newLit.Pixels[byte]) - int(newOccluded.Pixels[byte]) > threshold;
			released += oldMask && !newMask;
			newlyOccluded += newMask && !oldMask;
		}
		std::cout << "Occlusion movement released/new pixels " << released << '/' << newlyOccluded << '\n';
		Require(released > 15 && newlyOccluded > 15,
				"Motion did not remove old occlusion and create occlusion at the new location");
	}

	void RunSceneRenderingTests(Aster::RendererOptions options)
	{
		options.Width = 128;
		options.Height = 96;
		Aster::Renderer renderer(options);
		Aster::AssetImporter project(std::filesystem::path(ASTER_SOURCE_DIR) / "Assets");
		Aster::Scene scene("GPU feature checks");
		Aster::RenderSettings settings;
		settings.AmbientIntensity = 0;
		settings.BackgroundColor = {0, 0, 0};
		struct MotionReference
		{
			nlohmann::json Scene;
			Aster::RenderSettings Settings;
			Aster::RenderImage Image;
		};
		std::vector<MotionReference> motionReferences;
		const auto rememberMotion = [&](const Aster::RenderImage& image)
		{ motionReferences.push_back({scene.Serialize(), settings, image}); };
		RequireThrows<std::invalid_argument>([&]() { renderer.RenderScene(scene, project, settings); },
											 "Scene without primary camera was rendered");
		auto editorSettings = settings;
		Aster::RenderCamera editorCamera;
		editorCamera.View = glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0));
		editorCamera.Projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 128.0f / 96.0f, 0.1f, 100.0f);
		editorSettings.CameraOverride = editorCamera;
		renderer.RenderScene(scene, project, editorSettings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		editorSettings.CameraOverride->Projection[0][0] = std::numeric_limits<float>::quiet_NaN();
		RequireThrows<std::invalid_argument>([&]() { renderer.RenderScene(scene, project, editorSettings); },
											 "Nonfinite editor camera was accepted");
		editorSettings.CameraOverride->View = glm::mat4(1);
		editorSettings.CameraOverride->Projection = glm::mat4(1);
		editorSettings.CameraOverride->View[0][0] = 1.0e20f;
		editorSettings.CameraOverride->View[1][1] = 1.0e-20f;
		editorSettings.CameraOverride->Projection[0][0] = 1.0e20f;
		editorSettings.CameraOverride->Projection[1][1] = 1.0e-20f;
		RequireThrows<std::invalid_argument>(
			[&]() { renderer.RenderScene(scene, project, editorSettings); },
			"Overflowing product of individually invertible camera matrices was accepted");
		const auto camera = scene.CreateEntity("Camera");
		scene.Get(camera).Camera.emplace();
		scene.Get(camera).Transform.Translation = {0, 0, 3};
		const auto mesh = scene.CreateEntity("Mesh");
		scene.Get(mesh).MeshRenderer.emplace();
		scene.Get(mesh).MeshRenderer->Mesh = "Models/Triangle.gltf";
		const auto light = scene.CreateEntity("Light");
		scene.Get(light).Light.emplace();
		scene.Get(light).Light->Intensity = 4;
		renderer.RenderScene(scene, project, settings);
		const auto textured = renderer.ReadbackRgba8();
		const auto texturedCenter = CenterPixel(textured);
		Require(texturedCenter[0] + texturedCenter[1] + texturedCenter[2] > 20,
				"Imported textured triangle did not affect center pixels");
		scene.Get(mesh).MeshRenderer->Visible = false;
		renderer.RenderScene(scene, project, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(mesh).MeshRenderer->Visible = true;
		scene.Get(mesh).Transform.Translation.x = 10;
		renderer.RenderScene(scene, project, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(mesh).Transform.Translation.x = 0;

		const auto fixturePath =
			std::filesystem::path(ASTER_SOURCE_DIR) / "build" /
			("gpu-fixtures-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Require(std::filesystem::create_directory(fixturePath), "Cannot create GPU fixture directory");
		struct FixtureCleanup
		{
			std::filesystem::path Path;
			~FixtureCleanup()
			{
				std::error_code error;
				std::filesystem::remove_all(Path, error);
				if (error)
				{
					std::cerr << "GPU fixture cleanup failed: " << error.message() << '\n';
				}
			}
		} cleanup{fixturePath};
		std::ifstream source(std::filesystem::path(ASTER_SOURCE_DIR) / "Assets/Models/Triangle.gltf");
		nlohmann::json fixture;
		source >> fixture;
		fixture["materials"][0] = {
			{"pbrMetallicRoughness",
			 {{"baseColorFactor", {0, 0, 0, 1}}, {"metallicFactor", 0}, {"roughnessFactor", 0.5}}},
			{"emissiveFactor", {1.0, 0.125, 0.025}},
			{"extensions", {{"KHR_materials_emissive_strength", {{"emissiveStrength", 4.0}}}}},
			{"doubleSided", false}};
		fixture["extensionsUsed"] = {"KHR_materials_emissive_strength"};
		const auto writeFixture = [&](const char* name)
		{
			std::ofstream stream(fixturePath / name);
			stream << fixture.dump(2);
			Require(stream.good(), "Failed to write GPU glTF fixture");
		};
		writeFixture("emissive.gltf");
		Aster::AssetImporter fixtures(fixturePath);
		scene.Get(mesh).MeshRenderer->Mesh = "emissive.gltf";
		scene.Get(light).Light->Intensity = 0;
		for (const float exposure : {0.1f, 1.0f})
		{
			settings.Exposure = exposure;
			renderer.RenderScene(scene, fixtures, settings);
			const auto pixel = CenterPixel(renderer.ReadbackRgba8());
			std::cout << "HDR exposure " << exposure << " pixel " << static_cast<int>(pixel[0]) << ','
					  << static_cast<int>(pixel[1]) << ',' << static_cast<int>(pixel[2]) << '\n';
			const std::array<float, 3> radiance{4.0f, 0.5f, 0.1f};
			for (size_t channel = 0; channel < 3; ++channel)
			{
				Require(std::abs(static_cast<int>(pixel[channel]) - ToneMap(radiance[channel], exposure)) <= 3,
						"HDR emissive color or exposure differs from analytic tonemapping result");
			}
		}
		scene.Get(mesh).Transform.Scale.x = -1;
		renderer.RenderScene(scene, fixtures, settings);
		Require(CenterPixel(renderer.ReadbackRgba8())[0] > 200, "Mirrored transform incorrectly culled front faces");
		scene.Get(mesh).Transform.Scale.x = 1;
		scene.Get(mesh).Transform.Rotation.y = glm::pi<float>();
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(mesh).Transform.Rotation.y = 0;

		fixture["materials"][0].erase("extensions");
		fixture["materials"][0]["emissiveFactor"] = {0, 0, 0};
		fixture["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
		writeFixture("white.gltf");
		const auto motionFixture = fixture;
		scene.Get(mesh).MeshRenderer->Mesh = "white.gltf";
		scene.Get(light).Light->Intensity = 3;
		renderer.RenderScene(scene, fixtures, settings);
		const auto directional = renderer.ReadbackRgba8();
		Require(CenterPixel(directional)[0] > 150, "Directional PBR light did not illuminate mesh");
		Aster::Entity lightParent;
		Aster::Entity lightHierarchyRoot;
		for (uint32_t level = 0; level < 4; ++level)
		{
			const auto ancestor = scene.CreateEntity("Scaled light ancestor");
			scene.Get(ancestor).Transform.Scale.z = 1.0e-5f;
			if (lightParent)
			{
				scene.SetParent(ancestor, lightParent);
			}
			else
			{
				lightHierarchyRoot = ancestor;
			}
			lightParent = ancestor;
		}
		scene.SetParent(light, lightParent);
		scene.Get(light).Transform.Scale.z = 1.0e-5f;
		renderer.RenderScene(scene, fixtures, settings);
		Require(CenterPixel(renderer.ReadbackRgba8()) == CenterPixel(directional),
				"Light direction normalization underflowed through a valid scaled hierarchy");
		scene.SetParent(light, {});
		scene.Get(light).Transform.Scale.z = 1;
		scene.DestroyEntity(lightHierarchyRoot);

		scene.Get(light).Light->Intensity = 0;
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(light).Light->Intensity = 8;
		scene.Get(light).Light->Type = Aster::LightType::Point;
		scene.Get(light).Transform.Translation = {0, 0, 2};
		renderer.RenderScene(scene, fixtures, settings);
		Require(CenterPixel(renderer.ReadbackRgba8())[0] > 100, "Point light did not illuminate mesh");
		scene.Get(light).Light->Range = 0.5f;
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(light).Light->Range = 10;
		scene.Get(light).Light->Type = Aster::LightType::Spot;
		renderer.RenderScene(scene, fixtures, settings);
		Require(CenterPixel(renderer.ReadbackRgba8())[0] > 100, "Spot light did not illuminate its cone");
		scene.Get(light).Transform.Rotation.y = glm::pi<float>();
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 128, 96, {0, 0, 0, 1});
		scene.Get(light).Transform.Rotation.y = 0;

		const auto occluder = scene.CreateEntity("Shadow occluder");
		scene.Get(occluder).MeshRenderer.emplace();
		scene.Get(occluder).MeshRenderer->Mesh = "white.gltf";
		scene.Get(occluder).Transform.Translation = {0, 0, 0.65f};
		scene.Get(occluder).Transform.Scale = glm::vec3(0.32f);
		scene.Get(mesh).Transform.Scale = glm::vec3(2.0f);
		settings.ShadowResolution = 256;
		for (const auto type : {Aster::LightType::Directional, Aster::LightType::Point, Aster::LightType::Spot})
		{
			scene.Get(light).Light->Type = type;
			scene.Get(light).Light->Intensity = type == Aster::LightType::Directional ? 3 : 25;
			scene.Get(light).Transform.Translation = {1.5f, 0, 2};
			scene.Get(light).Transform.Rotation.y = 0.5f;
			scene.Get(light).Light->OuterCone = 55;
			scene.Get(light).Light->InnerCone = 40;
			settings.Shadows = false;
			renderer.RenderScene(scene, fixtures, settings);
			const auto unshadowed = renderer.ReadbackRgba8();
			settings.Shadows = true;
			renderer.RenderScene(scene, fixtures, settings);
			const auto shadowed = renderer.ReadbackRgba8();
			size_t darkened = 0;
			for (size_t byte = 0; byte < shadowed.Pixels.size(); byte += 4)
			{
				if (int(unshadowed.Pixels[byte]) - int(shadowed.Pixels[byte]) > 20)
				{
					++darkened;
				}
			}
			std::cout << "Shadow type " << static_cast<int>(type) << " darkened pixels " << darkened << '\n';
			Require(darkened > 15, "Shadow-casting light did not darken receiver pixels behind the occluder");
			const auto oldLightTransform = scene.Get(light).Transform;
			scene.Get(light).Transform.Translation.x = -1.5f;
			scene.Get(light).Transform.Rotation.y = -0.5f;
			renderer.RenderScene(scene, fixtures, settings);
			const auto movedLightShadow = renderer.ReadbackRgba8();
			settings.Shadows = false;
			renderer.RenderScene(scene, fixtures, settings);
			const auto movedLightLit = renderer.ReadbackRgba8();
			RequireOcclusionMoved(unshadowed, shadowed, movedLightLit, movedLightShadow, 20);
			scene.Get(occluder).Transform.Translation.x = -0.65f;
			renderer.RenderScene(scene, fixtures, settings);
			const auto movedOccluderLit = renderer.ReadbackRgba8();
			settings.Shadows = true;
			renderer.RenderScene(scene, fixtures, settings);
			const auto movedOccluderShadow = renderer.ReadbackRgba8();
			RequireOcclusionMoved(movedLightLit, movedLightShadow, movedOccluderLit, movedOccluderShadow, 20);
			rememberMotion(movedOccluderShadow);
			scene.Get(light).Transform = oldLightTransform;
			scene.Get(occluder).Transform.Translation.x = 0;
			renderer.RenderScene(scene, fixtures, settings);
			Require(renderer.ReadbackRgba8().Pixels == shadowed.Pixels,
					"Restoring light and occluder transforms retained stale shadow data");
			scene.Get(light).Light->CastShadows = false;
			renderer.RenderScene(scene, fixtures, settings);
			Require(renderer.ReadbackRgba8().Pixels == unshadowed.Pixels, "CastShadows=false retained a shadow");
			scene.Get(light).Light->CastShadows = true;
			settings.ShadowSoftness = 0;
			renderer.RenderScene(scene, fixtures, settings);
			Require(renderer.ReadbackRgba8().Pixels != shadowed.Pixels, "PCF softness did not change shadow edges");
			scene.Get(occluder).MeshRenderer->Visible = false;
			settings.Shadows = false;
			renderer.RenderScene(scene, fixtures, settings);
			const auto clearReceiver = renderer.ReadbackRgba8();
			settings.Shadows = true;
			renderer.RenderScene(scene, fixtures, settings);
			const auto selfShadow = renderer.ReadbackRgba8();
			size_t artifacts = 0;
			for (size_t byte = 0; byte < selfShadow.Pixels.size(); byte += 4)
			{
				if (int(clearReceiver.Pixels[byte]) - int(selfShadow.Pixels[byte]) > 10)
				{
					++artifacts;
				}
			}
			std::cout << "Self-shadow artifacts " << artifacts << '\n';
			Require(artifacts < 10, "Isolated flat receiver exhibits self-shadow acne");
			scene.Get(occluder).MeshRenderer->Visible = true;

			settings.ShadowSoftness = 1.5f;
		}
		settings.Shadows = false;
		settings.AmbientIntensity = 0.5f;
		scene.Get(light).Light->Intensity = 0;
		scene.Get(occluder).Transform.Translation.z = 0.15f;
		settings.AmbientOcclusion = false;
		renderer.RenderScene(scene, fixtures, settings);
		const auto ambientWithoutAo = renderer.ReadbackRgba8();
		settings.AmbientOcclusion = true;
		renderer.RenderScene(scene, fixtures, settings);
		const auto ambientWithAo = renderer.ReadbackRgba8();
		size_t occludedPixels = 0;
		for (size_t byte = 0; byte < ambientWithAo.Pixels.size(); byte += 4)
		{
			if (int(ambientWithoutAo.Pixels[byte]) - int(ambientWithAo.Pixels[byte]) > 8)
			{
				++occludedPixels;
			}
			Require(ambientWithAo.Pixels[byte] <= ambientWithoutAo.Pixels[byte] + 1,
					"SSAO added light instead of attenuating ambient light");
		}
		std::cout << "SSAO darkened pixels " << occludedPixels << '\n';
		Require(occludedPixels > 15, "SSAO did not darken ambient lighting near the occluder");
		scene.Get(occluder).Transform.Translation.x = 0.65f;
		renderer.RenderScene(scene, fixtures, settings);
		const auto movedAo = renderer.ReadbackRgba8();
		rememberMotion(movedAo);
		settings.AmbientOcclusion = false;
		renderer.RenderScene(scene, fixtures, settings);
		RequireOcclusionMoved(ambientWithoutAo, ambientWithAo, renderer.ReadbackRgba8(), movedAo, 8);
		settings.AmbientOcclusion = true;
		scene.Get(occluder).Transform.Translation.x = 2.1f;
		renderer.RenderScene(scene, fixtures, settings);
		const auto edgeAo = renderer.ReadbackRgba8();
		rememberMotion(edgeAo);
		scene.Get(occluder).Transform.Translation.x = 3.0f;
		renderer.RenderScene(scene, fixtures, settings);
		const auto offscreenAo = renderer.ReadbackRgba8();
		scene.Get(occluder).MeshRenderer->Visible = false;
		renderer.RenderScene(scene, fixtures, settings);
		const auto flatAo = renderer.ReadbackRgba8();
		Require(offscreenAo.Pixels == flatAo.Pixels, "Offscreen occluder left stale SSAO on the receiver");
		Require(edgeAo.Pixels != flatAo.Pixels, "SSAO edge-motion fixture did not intersect the viewport");
		settings.AmbientOcclusion = false;
		renderer.RenderScene(scene, fixtures, settings);
		Require(renderer.ReadbackRgba8().Pixels == flatAo.Pixels, "SSAO darkened an isolated flat receiver");
		scene.Get(occluder).MeshRenderer->Visible = true;
		scene.Get(occluder).Transform.Translation.x = 0;
		settings.AmbientOcclusion = true;
		renderer.RenderScene(scene, fixtures, settings);
		Require(renderer.ReadbackRgba8().Pixels == ambientWithAo.Pixels,
				"Restoring the occluder transform retained stale SSAO or depth data");
		settings.AmbientOcclusion = false;
		settings.AmbientIntensity = 0;
		scene.Get(light).Light->Intensity = 3;
		scene.Get(light).Light->Type = Aster::LightType::Directional;
		renderer.RenderScene(scene, fixtures, settings);
		const auto directWithoutAo = renderer.ReadbackRgba8();
		settings.AmbientOcclusion = true;
		renderer.RenderScene(scene, fixtures, settings);
		Require(renderer.ReadbackRgba8().Pixels == directWithoutAo.Pixels,
				"SSAO incorrectly attenuated direct lighting");
		auto invalidAoSettings = settings;
		invalidAoSettings.AmbientOcclusionRadius = std::numeric_limits<float>::quiet_NaN();
		RequireThrows<std::invalid_argument>([&]() { renderer.RenderScene(scene, fixtures, invalidAoSettings); },
											 "Nonfinite ambient occlusion radius was accepted");
		settings.Shadows = true;
		scene.DestroyEntity(occluder);
		scene.Get(mesh).Transform.Scale = glm::vec3(1.0f);
		auto invalidShadowSettings = settings;
		invalidShadowSettings.ShadowResolution = 0;
		RequireThrows<std::invalid_argument>([&]() { renderer.RenderScene(scene, fixtures, invalidShadowSettings); },
											 "Zero-resolution shadow maps were accepted");
		RequireClean(renderer);

		fixture["materials"][0]["emissiveFactor"] = {0, 0, 1};
		fixture["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 0, 1};
		writeFixture("white.gltf");
		renderer.InvalidateAssets();
		scene.Get(light).Light->Intensity = 0;
		renderer.RenderScene(scene, fixtures, settings);
		const auto reloaded = CenterPixel(renderer.ReadbackRgba8());
		Require(reloaded[2] > 200 && reloaded[0] < 3 && reloaded[1] < 3,
				"Asset invalidation did not replace GPU material");
		renderer.Resize(96, 80);
		renderer.RenderScene(scene, fixtures, settings);
		Require(CenterPixel(renderer.ReadbackRgba8())[2] > 200, "HDR scene pass failed after resize");
		Aster::HDRImageAsset constantSky;
		constantSky.Width = 8;
		constantSky.Height = 4;
		constantSky.Pixels.resize(8 * 4 * 4);
		for (size_t pixel = 0; pixel < constantSky.Pixels.size(); pixel += 4)
		{
			constantSky.Pixels[pixel] = 0.5f;
			constantSky.Pixels[pixel + 1] = 0.25f;
			constantSky.Pixels[pixel + 2] = 0.125f;
			constantSky.Pixels[pixel + 3] = 1.0f;
		}
		Aster::EnvironmentSettings environmentSettings;
		environmentSettings.IrradianceWidth = 4;
		environmentSettings.IrradianceHeight = 2;
		environmentSettings.SpecularWidth = 8;
		environmentSettings.SpecularHeight = 4;
		environmentSettings.SpecularMipCount = 3;
		environmentSettings.BRDFSize = 8;
		environmentSettings.SampleCount = 32;
		const auto environment = Aster::EnvironmentProcessor::Build(constantSky, environmentSettings);
		auto invalidEnvironment = environment;
		invalidEnvironment.Irradiance.Pixels[0] = std::numeric_limits<float>::quiet_NaN();
		RequireThrows<std::invalid_argument>([&]() { renderer.SetEnvironment(invalidEnvironment); },
											 "Nonfinite environment radiance was accepted");
		renderer.SetEnvironment(environment);
		settings.UseSceneEnvironment = false;
		scene.Get(mesh).MeshRenderer->Visible = false;
		renderer.RenderScene(scene, fixtures, settings);
		const auto skyPixel = CenterPixel(renderer.ReadbackRgba8());
		for (size_t channel = 0; channel < 3; ++channel)
		{
			Require(std::abs(static_cast<int>(skyPixel[channel]) - ToneMap(constantSky.Pixels[channel], 1.0f)) <= 3,
					"HDR sky rendering differs from its input radiance");
		}
		settings.DrawSky = false;
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 96, 80, {0, 0, 0, 1});
		fixture["materials"][0]["emissiveFactor"] = {0, 0, 0};
		fixture["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
		writeFixture("white.gltf");
		renderer.InvalidateAssets();
		scene.Get(mesh).MeshRenderer->Visible = true;
		renderer.RenderScene(scene, fixtures, settings);
		const auto iblPixel = CenterPixel(renderer.ReadbackRgba8());
		Require(iblPixel[0] > 150 && iblPixel[1] > 100 && iblPixel[0] > iblPixel[2],
				"Irradiance/specular IBL did not illuminate an unlit scene");
		settings.EnvironmentIntensity = 0;
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 96, 80, {0, 0, 0, 1});
		settings.EnvironmentIntensity = 1;
		settings.UseSceneEnvironment = true;
		renderer.RenderScene(scene, fixtures, settings);
		RequireColor(renderer.ReadbackRgba8(), 96, 80, {0, 0, 0, 1});
		scene.Get(mesh).MeshRenderer->Visible = false;
		settings.DrawSky = true;
		scene.SetEnvironment({"Environment/StudioSmall09.hdr", 0.5f, 0.0f});
		renderer.RenderScene(scene, project, settings);
		const auto polyHavenSky = renderer.ReadbackRgba8();
		scene.SetEnvironment({"Environment/StudioSmall09.hdr", 0.5f, 1.7f});
		renderer.RenderScene(scene, project, settings);
		Require(renderer.ReadbackRgba8().Pixels != polyHavenSky.Pixels,
				"Persisted environment yaw did not rotate the Poly Haven sky");
		scene.SetEnvironment({"Environment/StudioSmall09.hdr", 0.0f, 1.7f});
		renderer.RenderScene(scene, project, settings);
		RequireColor(renderer.ReadbackRgba8(), 96, 80, {0, 0, 0, 1});
		scene.SetEnvironment({});
		scene.Get(mesh).MeshRenderer->Visible = true;
		scene.Get(mesh).Transform = {};
		scene.Get(light).Light->Type = Aster::LightType::Directional;
		scene.Get(light).Light->Intensity = 0;
		scene.Get(light).Transform = {};
		settings.AmbientIntensity = 0;
		const std::array<const char*, 7> imageUris{
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNoaGj4DwAFhAKAjM1mJgAAAABJRU5ErkJggg==",
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNoaPj/HwAGggL/s75RMwAAAABJRU5ErkJggg==",
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP439DwHwAHgAL/OfPOjwAAAABJRU5ErkJggg==",
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=",
			"iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR4nGNgYGD4DwJgEsQBAFa7CfdqxQ/7AAAAAElFTkSuQmCC",
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP438DwHwAGgAJ/EEwb4QAAAABJRU5ErkJggg==",
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP43/D/PwAIfgN+VfwVfQAAAABJRU5ErkJggg=="};
		fixture["images"] = nlohmann::json::array();
		fixture["textures"] = nlohmann::json::array();
		fixture["samplers"] = {{{"minFilter", 9984}, {"magFilter", 9728}}};
		fixture["extensionsUsed"] = {"KHR_materials_unlit", "KHR_texture_transform"};
		for (size_t index = 0; index < imageUris.size(); ++index)
		{
			fixture["images"].push_back({{"uri", std::string("data:image/png;base64,") + imageUris[index]}});
			fixture["textures"].push_back({{"source", index}, {"sampler", 0}});
		}
		const nlohmann::json whiteMaterial = {
			{"pbrMetallicRoughness",
			 {{"baseColorFactor", {1, 1, 1, 1}}, {"metallicFactor", 0}, {"roughnessFactor", 0.5}}}};
		const auto renderMaterial = [&](const nlohmann::json& material)
		{
			fixture["materials"][0] = material;
			writeFixture("material.gltf");
			renderer.InvalidateAssets();
			scene.Get(mesh).MeshRenderer->Mesh = "material.gltf";
			renderer.RenderScene(scene, fixtures, settings);
			return renderer.ReadbackRgba8();
		};
		const auto nearPixel = [&](const std::array<uint8_t, 3>& pixel, int value, const char* message)
		{
			for (const auto channel : pixel)
			{
				Require(std::abs(int(channel) - value) <= 3, message);
			}
		};
		auto material = whiteMaterial;
		material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
		material["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 0}};
		const float grayLinear = std::pow((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f);
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(grayLinear, 1),
				  "Base color texture was not decoded from sRGB");
		material["pbrMetallicRoughness"]["baseColorTexture"] = {
			{"index", 4}, {"extensions", {{"KHR_texture_transform", {{"scale", {256, 256}}}}}}};
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(0.5f, 1),
				  "sRGB mipmaps were averaged in gamma space or not selected");
		material = whiteMaterial;
		material["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 0, 1};
		material["emissiveFactor"] = {1, 1, 1};
		material["emissiveTexture"] = {{"index", 0}};
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(grayLinear, 1),
				  "Emissive texture was not decoded from sRGB");
		material = whiteMaterial;
		settings.AmbientIntensity = 0.5f;
		material["occlusionTexture"] = {{"index", 0}, {"strength", 1}};
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(0.5f * 128.0f / 255.0f, 1),
				  "Occlusion red channel was not sampled in linear space");
		material["occlusionTexture"]["strength"] = 0;
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(0.5f, 1),
				  "Occlusion strength zero still attenuated ambient light");
		settings.AmbientIntensity = 0;
		scene.Get(light).Light->Intensity = 3;
		material = whiteMaterial;
		material["normalTexture"] = {{"index", 1}};
		const auto flatNormal = CenterPixel(renderMaterial(material));
		material["normalTexture"]["index"] = 2;
		const auto sideNormal = CenterPixel(renderMaterial(material));
		Require(flatNormal[0] > sideNormal[0] + 80, "Tangent normal texture did not rotate direct lighting");
		material["normalTexture"]["scale"] = 0;
		const auto zeroNormalScale = CenterPixel(renderMaterial(material));
		Require(std::abs(int(flatNormal[0]) - int(zeroNormalScale[0])) <= 3,
				"Normal scale zero did not restore the geometric normal");
		material = whiteMaterial;
		material["pbrMetallicRoughness"]["baseColorFactor"] = {0.7f, 0.2f, 0.1f, 1};
		material["pbrMetallicRoughness"]["roughnessFactor"] = 128.0f / 255.0f;
		const auto dielectric = CenterPixel(renderMaterial(material));
		material["pbrMetallicRoughness"]["metallicFactor"] = 1;
		material["pbrMetallicRoughness"]["roughnessFactor"] = 1;
		material["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {{"index", 5}};
		const auto texturedDielectric = CenterPixel(renderMaterial(material));
		Require(dielectric == texturedDielectric, "Metallic/roughness texture did not use linear blue/green channels");
		material["pbrMetallicRoughness"]["metallicRoughnessTexture"]["index"] = 6;
		const auto texturedMetal = CenterPixel(renderMaterial(material));
		material["pbrMetallicRoughness"].erase("metallicRoughnessTexture");
		material["pbrMetallicRoughness"]["roughnessFactor"] = 128.0f / 255.0f;
		const auto metal = CenterPixel(renderMaterial(material));
		Require(metal == texturedMetal && metal != dielectric,
				"Metallic texture blue channel did not produce the corresponding metal material");
		material = whiteMaterial;
		material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
		material["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 0.25f};
		material["alphaMode"] = "MASK";
		material["alphaCutoff"] = 0.5f;
		RequireColor(renderMaterial(material), 96, 80, {0, 0, 0, 1});
		material["alphaCutoff"] = 0.1f;
		Require(CenterPixel(renderMaterial(material))[0] > 200, "Alpha mask discarded a fragment above cutoff");
		material["alphaMode"] = "BLEND";
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(0.25f, 1),
				  "Alpha blend did not composite in HDR linear space");
		material["alphaMode"] = "OPAQUE";
		material["doubleSided"] = true;
		scene.Get(mesh).Transform.Rotation.y = glm::pi<float>();
		Require(CenterPixel(renderMaterial(material))[0] > 200, "Double-sided material culled its back face");
		scene.Get(mesh).Transform = {};
		std::cout
			<< "Validated material sRGB/linear channels, mipmaps, normal maps, alpha mask/blend, double-sided output\n";
		const auto uvBufferIndex = fixture["buffers"].size();
		const auto uvViewIndex = fixture["bufferViews"].size();
		const auto uvAccessorIndex = fixture["accessors"].size();
		fixture["buffers"].push_back({{"uri", "uv1.bin"}, {"byteLength", 24}});
		fixture["bufferViews"].push_back({{"buffer", uvBufferIndex}, {"byteLength", 24}});
		fixture["accessors"].push_back(
			{{"bufferView", uvViewIndex}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}});
		fixture["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = uvAccessorIndex;
		{
			const std::array<float, 6> coordinates{0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
			std::ofstream uvFile(fixturePath / "uv1.bin", std::ios::binary);
			for (const auto coordinate : coordinates)
			{
				const auto word = std::bit_cast<uint32_t>(coordinate);
				for (uint32_t byte = 0; byte < 4; ++byte)
				{
					uvFile.put(static_cast<char>((word >> (8 * byte)) & 255));
				}
			}
			Require(uvFile.good(), "Could not write secondary-UV fixture");
		}
		material = whiteMaterial;
		material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
		material["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 4}, {"texCoord", 1}};
		RequireColor(renderMaterial(material), 96, 80, {0, 0, 0, 1});
		material["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"] = {
			{"offset", {0.5f, 0.0f}}};
		nearPixel(CenterPixel(renderMaterial(material)), ToneMap(1, 1),
				  "Secondary UV set or texture offset did not select the white texel");
		Aster::HDRImageAsset highlight;
		highlight.Width = 32;
		highlight.Height = 16;
		highlight.Pixels.resize(32 * 16 * 4, 1.0f);
		for (uint32_t y = 0; y < highlight.Height; ++y)
		{
			for (uint32_t x = 0; x < highlight.Width; ++x)
			{
				const float radiance = x >= 22 && x <= 25 && y >= 6 && y <= 9 ? 8.0f : 0.01f;
				for (uint32_t channel = 0; channel < 3; ++channel)
				{
					highlight.Pixels[(y * 32 + x) * 4 + channel] = radiance;
				}
			}
		}
		Aster::EnvironmentSettings highlightSettings;
		highlightSettings.IrradianceWidth = 8;
		highlightSettings.IrradianceHeight = 4;
		highlightSettings.SpecularWidth = 32;
		highlightSettings.SpecularHeight = 16;
		highlightSettings.SpecularMipCount = 6;
		highlightSettings.BRDFSize = 16;
		highlightSettings.SampleCount = 128;
		renderer.SetEnvironment(Aster::EnvironmentProcessor::Build(highlight, highlightSettings));
		settings.UseSceneEnvironment = false;
		settings.DrawSky = false;
		scene.Get(light).Light->Intensity = 0;
		material = whiteMaterial;
		material["pbrMetallicRoughness"]["metallicFactor"] = 1;
		material["pbrMetallicRoughness"]["roughnessFactor"] = 0.05f;
		const auto smoothIbl = CenterPixel(renderMaterial(material));
		material["pbrMetallicRoughness"]["roughnessFactor"] = 1;
		const auto roughIbl = CenterPixel(renderMaterial(material));
		std::cout << "IBL highlight smooth/rough " << int(smoothIbl[0]) << '/' << int(roughIbl[0]) << '\n';
		Require(smoothIbl[0] > roughIbl[0] + 30, "IBL roughness did not sample the broader prefiltered mip levels");
		RequireClean(renderer);
		renderer.Shutdown();
		RequireClean(renderer);
		{
			std::ofstream stream(fixturePath / "white.gltf");
			stream << motionFixture.dump(2);
			Require(stream.good(), "Failed to restore the motion reference asset");
		}
		for (const auto& reference : motionReferences)
		{
			auto referenceScene = Aster::Scene::Deserialize(reference.Scene);
			Aster::Renderer freshRenderer(options);
			freshRenderer.RenderScene(referenceScene, fixtures, reference.Settings);
			Require(freshRenderer.ReadbackRgba8().Pixels == reference.Image.Pixels,
					"Reused shadow/SSAO passes differ from a fresh renderer at the same final scene state");
			freshRenderer.Shutdown();
			RequireClean(freshRenderer);
		}
		std::cout << "Validated moving lights, shadow casters, SSAO viewport edges, and " << motionReferences.size()
				  << " fresh-renderer motion references\n";
		std::cout << "Validated glTF textures, camera, transforms, PBR lights, HDR exposure, IBL, sky, asset reload, "
					 "shadows, SSAO, and scene "
					 "resize\n";
	}
} // namespace
