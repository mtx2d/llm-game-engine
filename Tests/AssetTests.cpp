#include "Aster/Assets/AssetImporter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>

namespace
{
	using Json = nlohmann::json;

	void Check(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error("Asset test failed: " + message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const std::string& message)
	{
		bool rejected = false;
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Check(rejected, message);
	}

	class TemporaryAssets
	{
	  public:
		TemporaryAssets()
		{
			std::random_device random;
			Path = std::filesystem::current_path() / ("AssetTestArtifacts-" + std::to_string(random()));
			std::filesystem::create_directories(Path / "Project" / "Meshes");
			std::filesystem::create_directories(Path / "Project" / "Data");
			std::filesystem::create_directories(Path / "Project" / "Textures");
		}

		~TemporaryAssets()
		{
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}

		std::filesystem::path Path;
	};

	void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::ofstream output(path, std::ios::binary);
		output.exceptions(std::ios::badbit | std::ios::failbit);
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	void WriteJson(const std::filesystem::path& path, const Json& document)
	{
		std::ofstream output(path);
		output.exceptions(std::ios::badbit | std::ios::failbit);
		output << document.dump(2);
	}

	std::vector<uint8_t> DecodeFixtureURI(const std::string& uri)
	{
		const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::vector<uint8_t> bytes;
		uint32_t bits = 0;
		int bitCount = 0;
		for (const auto character : uri.substr(uri.find(',') + 1))
		{
			if (character == '=')
			{
				break;
			}
			const auto digit = alphabet.find(character);
			Check(digit != std::string::npos, "fixture base64 is valid");
			bits = (bits << 6) | static_cast<uint32_t>(digit);
			bitCount += 6;
			if (bitCount >= 8)
			{
				bitCount -= 8;
				bytes.push_back(static_cast<uint8_t>(bits >> bitCount));
			}
		}
		return bytes;
	}

	std::string EncodeBuffer(const std::vector<uint8_t>& bytes)
	{
		const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string encoded = "data:application/octet-stream;base64,";
		uint32_t bits = 0;
		int bitCount = 0;
		for (const auto byte : bytes)
		{
			bits = (bits << 8) | byte;
			bitCount += 8;
			while (bitCount >= 6)
			{
				bitCount -= 6;
				encoded += alphabet[(bits >> bitCount) & 63];
			}
		}
		if (bitCount != 0)
		{
			encoded += alphabet[(bits << (6 - bitCount)) & 63];
		}
		while ((encoded.size() - encoded.find(',') - 1) % 4 != 0)
		{
			encoded += '=';
		}
		return encoded;
	}

	void AppendWord(std::vector<uint8_t>& bytes, uint32_t word)
	{
		for (int byte = 0; byte < 4; ++byte)
		{
			bytes.push_back(static_cast<uint8_t>(word >> (byte * 8)));
		}
	}

	void StoreFloat(std::vector<uint8_t>& bytes, size_t offset, float value)
	{
		const auto word = std::bit_cast<uint32_t>(value);
		for (size_t byte = 0; byte < 4; ++byte)
		{
			bytes.at(offset + byte) = static_cast<uint8_t>(word >> (byte * 8));
		}
	}

	std::vector<uint8_t> MakeGLB(Json document, std::vector<uint8_t> binary)
	{
		document["buffers"][0].erase("uri");
		std::string json = document.dump();
		while (json.size() % 4 != 0)
		{
			json += ' ';
		}
		while (binary.size() % 4 != 0)
		{
			binary.push_back(0);
		}
		std::vector<uint8_t> glb;
		AppendWord(glb, 0x46546c67);
		AppendWord(glb, 2);
		AppendWord(glb, static_cast<uint32_t>(28 + json.size() + binary.size()));
		AppendWord(glb, static_cast<uint32_t>(json.size()));
		AppendWord(glb, 0x4e4f534a);
		glb.insert(glb.end(), json.begin(), json.end());
		AppendWord(glb, static_cast<uint32_t>(binary.size()));
		AppendWord(glb, 0x004e4942);
		glb.insert(glb.end(), binary.begin(), binary.end());
		return glb;
	}

	void CheckTriangle(const Aster::MeshAsset& mesh)
	{
		Check(mesh.Primitives.size() == 1, "triangle has one primitive");
		const auto& primitive = mesh.Primitives.front();
		Check(primitive.Vertices.size() == 3 && primitive.Indices == std::vector<uint32_t>{0, 1, 2},
			  "indexed vertices decoded");
		Check(primitive.Vertices[0].Position == glm::vec3(-1.0f, -1.0f, 0.0f) &&
				  primitive.Vertices[2].Position == glm::vec3(0.0f, 1.0f, 0.0f),
			  "interleaved positions decoded");
		Check(primitive.Vertices[1].TexCoord == glm::vec2(1.0f, 0.0f), "interleaved texture coordinates decoded");
		Check(primitive.Vertices[0].Normal == glm::vec3(0.0f, 0.0f, 1.0f) &&
				  primitive.Vertices[0].Tangent == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
			  "normal and tangent basis decoded");
		const auto& material = mesh.Materials.at(primitive.MaterialIndex);
		Check(material.MetallicFactor == 0.25f && material.RoughnessFactor == 0.65f &&
				  material.BaseColorFactor == glm::vec4(0.8f, 0.6f, 0.4f, 1.0f),
			  "PBR factors preserved");
		Check(material.BaseColorTexture.SRGB && material.EmissiveTexture.SRGB && !material.NormalTexture.SRGB &&
				  !material.MetallicRoughnessTexture.SRGB && !material.OcclusionTexture.SRGB,
			  "color space is chosen per texture usage");
		Check(material.NormalTexture.Scale == 0.5f && material.OcclusionTexture.Scale == 0.75f &&
				  material.AlphaMode == Aster::MaterialAlphaMode::Mask && material.AlphaCutoff == 0.3f &&
				  material.DoubleSided,
			  "normal scale, occlusion strength and alpha behavior preserved");
		const auto& texture = mesh.Textures.at(0);
		Check(texture.Image.Width == 2 && texture.Image.Height == 2 && texture.Image.Pixels.size() == 16,
			  "embedded PNG dimensions decoded");
		Check(texture.Image.Pixels ==
				  std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255},
			  "RGBA texture contents and row order preserved");
		Check(texture.MinFilter == 9987 && texture.MagFilter == 9729 && texture.WrapV == 33071,
			  "sampler behavior preserved");
	}
} // namespace

void RunAssetTests()
{
	const auto fixturePath = std::filesystem::path(ASTER_SOURCE_DIR) / "Assets" / "Models" / "Triangle.gltf";
	std::ifstream fixtureFile(fixturePath);
	Json fixture;
	fixtureFile >> fixture;
	const auto geometry = DecodeFixtureURI(fixture["buffers"][0]["uri"]);
	const auto png = DecodeFixtureURI(fixture["images"][0]["uri"]);
	TemporaryAssets temporary;
	const auto root = temporary.Path / "Project";
	Aster::AssetImporter importer(root);
	const auto modelPath = root / "Meshes" / "Triangle.gltf";
	WriteJson(modelPath, fixture);
	CheckTriangle(importer.LoadMesh("Meshes/Triangle.gltf"));

	auto glbDocument = fixture;
	auto glbBuffer = geometry;
	while (glbBuffer.size() % 4 != 0)
	{
		glbBuffer.push_back(0);
	}
	glbDocument["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", glbBuffer.size()}, {"byteLength", png.size()}});
	glbDocument["images"][0] = {{"bufferView", 2}, {"mimeType", "image/png"}};
	glbBuffer.insert(glbBuffer.end(), png.begin(), png.end());
	glbDocument["buffers"][0]["byteLength"] = glbBuffer.size();
	const auto glb = MakeGLB(glbDocument, glbBuffer);
	WriteBytes(root / "Meshes" / "Triangle.glb", glb);
	CheckTriangle(importer.LoadMesh("Meshes/Triangle.glb"));

	auto external = fixture;
	external["buffers"][0]["uri"] = "../Data/Vertices%20one.bin";
	external["images"][0]["uri"] = "../Textures/Color%20one.png";
	WriteBytes(root / "Data" / "Vertices one.bin", geometry);
	WriteBytes(root / "Textures" / "Color one.png", png);
	WriteJson(modelPath, external);
	const auto externalMesh = importer.LoadMesh("Meshes/Triangle.gltf");
	CheckTriangle(externalMesh);
	Check(externalMesh.Dependencies.size() == 3 &&
			  std::find(externalMesh.Dependencies.begin(), externalMesh.Dependencies.end(), "Data/Vertices one.bin") !=
				  externalMesh.Dependencies.end(),
		  "export dependency closure includes external buffer and image");
	Check(importer.LoadImage("Textures/Color one.png").Pixels == externalMesh.Textures[0].Image.Pixels,
		  "standalone image decoding");

	auto hierarchy = fixture;
	hierarchy["nodes"] = Json::array({{{"translation", {1, 2, 3}}, {"scale", {2, 2, 2}}, {"children", {1}}},
									  {{"name", "Child"}, {"mesh", 0}, {"translation", {3, 0, 0}}},
									  {{"name", "Instance"}, {"mesh", 0}, {"translation", {-1, 0, 0}}}});
	hierarchy["scenes"][0]["nodes"] = {0, 2};
	WriteJson(modelPath, hierarchy);
	const auto transformed = importer.LoadMesh("Meshes/Triangle.gltf");
	Check(transformed.Primitives.size() == 2, "each mesh instance is imported");
	for (const auto& primitive : transformed.Primitives)
	{
		const auto expected =
			primitive.NodeName == "Child" ? glm::vec3(7.0f, 2.0f, 3.0f) : glm::vec3(-1.0f, 0.0f, 0.0f);
		Check(glm::vec3(primitive.Transform[3]) == expected, "hierarchical world transform preserved");
	}

	auto transformedUV = fixture;
	transformedUV["extensionsRequired"] = {"KHR_texture_transform", "KHR_materials_emissive_strength",
										   "KHR_materials_unlit"};
	transformedUV["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"] = {
		{"offset", {0.1, 0.2}}, {"scale", {2, 3}}, {"rotation", 0}};
	transformedUV["materials"][0]["extensions"] = {{"KHR_materials_emissive_strength", {{"emissiveStrength", 2}}},
												   {"KHR_materials_unlit", Json::object()}};
	WriteJson(modelPath, transformedUV);
	const auto extended = importer.LoadMesh("Meshes/Triangle.gltf");
	const auto uv = extended.Materials[0].BaseColorTexture.Transform * glm::vec3(0.5f, 0.5f, 1.0f);
	Check(glm::length(uv - glm::vec3(1.1f, 1.7f, 1.0f)) < 0.0001f, "KHR_texture_transform composed");
	Check(extended.Materials[0].Unlit && extended.Materials[0].EmissiveFactor == glm::vec3(0.2f, 0.4f, 0.6f),
		  "material extensions preserved");

	auto generated = fixture;
	generated["meshes"][0]["primitives"][0]["attributes"].erase("NORMAL");
	generated["meshes"][0]["primitives"][0]["attributes"].erase("TANGENT");
	generated["meshes"][0]["primitives"][0].erase("indices");
	WriteJson(modelPath, generated);
	CheckTriangle(importer.LoadMesh("Meshes/Triangle.gltf"));

	for (const int componentType : {5121, 5123, 5125})
	{
		auto indices = fixture;
		std::vector<uint8_t> buffer(geometry.begin(), geometry.begin() + 144);
		const size_t elementSize = componentType == 5121 ? 1 : componentType == 5123 ? 2 : 4;
		for (uint32_t index = 0; index < 3; ++index)
		{
			for (size_t byte = 0; byte < elementSize; ++byte)
			{
				buffer.push_back(static_cast<uint8_t>(index >> (byte * 8)));
			}
		}
		indices["accessors"][4]["componentType"] = componentType;
		indices["bufferViews"][1]["byteLength"] = elementSize * 3;
		indices["buffers"][0] = {{"byteLength", buffer.size()}, {"uri", EncodeBuffer(buffer)}};
		WriteJson(modelPath, indices);
		CheckTriangle(importer.LoadMesh("Meshes/Triangle.gltf"));
	}

	auto sparse = fixture;
	auto sparseBuffer = geometry;
	sparseBuffer.push_back(0);
	sparseBuffer.push_back(2);
	for (const float value : {-1.0f, -1.0f, 0.0f, 0.0f, 2.0f, 0.0f})
	{
		AppendWord(sparseBuffer, std::bit_cast<uint32_t>(value));
	}
	sparse["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", 150}, {"byteLength", 2}});
	sparse["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", 152}, {"byteLength", 24}});
	sparse["accessors"][0]["sparse"] = {
		{"count", 2}, {"indices", {{"bufferView", 2}, {"componentType", 5121}}}, {"values", {{"bufferView", 3}}}};
	sparse["buffers"][0] = {{"byteLength", sparseBuffer.size()}, {"uri", EncodeBuffer(sparseBuffer)}};
	WriteJson(modelPath, sparse);
	Check(importer.LoadMesh("Meshes/Triangle.gltf").Primitives[0].Vertices[2].Position.y == 2.0f,
		  "sparse position overrides applied");

	auto normalized = fixture;
	auto normalizedBuffer = geometry;
	for (size_t index = 0; index < 3; ++index)
	{
		const uint16_t u = index == 0 ? 0 : index == 1 ? 65535 : 32768;
		const uint16_t v = index == 2 ? 65535 : 0;
		normalizedBuffer[index * 48 + 24] = static_cast<uint8_t>(u);
		normalizedBuffer[index * 48 + 25] = static_cast<uint8_t>(u >> 8);
		normalizedBuffer[index * 48 + 26] = static_cast<uint8_t>(v);
		normalizedBuffer[index * 48 + 27] = static_cast<uint8_t>(v >> 8);
	}
	normalized["accessors"][2]["componentType"] = 5123;
	normalized["accessors"][2]["normalized"] = true;
	normalized["buffers"][0]["uri"] = EncodeBuffer(normalizedBuffer);
	WriteJson(modelPath, normalized);
	CheckTriangle(importer.LoadMesh("Meshes/Triangle.gltf"));

	const std::vector<std::pair<std::string, std::function<void(Json&)>>> invalidCases = {
		{"unsupported version", [](Json& doc) { doc["asset"]["version"] = "1.0"; }},
		{"required extension", [](Json& doc) { doc["extensionsRequired"] = {"UNKNOWN_extension"}; }},
		{"unsupported primitive", [](Json& doc) { doc["meshes"][0]["primitives"][0]["mode"] = 1; }},
		{"missing position", [](Json& doc) { doc["meshes"][0]["primitives"][0]["attributes"].erase("POSITION"); }},
		{"wrong position shape", [](Json& doc) { doc["accessors"][0]["type"] = "VEC2"; }},
		{"fractional accessor count", [](Json& doc) { doc["accessors"][0]["count"] = 3.75; }},
		{"boolean accessor count", [](Json& doc) { doc["accessors"][0]["count"] = true; }},
		{"string accessor count", [](Json& doc) { doc["accessors"][0]["count"] = "3"; }},
		{"fractional buffer offset", [](Json& doc) { doc["bufferViews"][0]["byteOffset"] = 0.5; }},
		{"fractional primitive mode", [](Json& doc) { doc["meshes"][0]["primitives"][0]["mode"] = 4.75; }},
		{"string attribute index",
		 [](Json& doc) { doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"] = "0"; }},
		{"number normalized flag", [](Json& doc) { doc["accessors"][0]["normalized"] = 0; }},
		{"boolean translation", [](Json& doc) { doc["nodes"][0]["translation"] = {true, 0, 0}; }},
		{"boolean roughness", [](Json& doc) { doc["materials"][0]["pbrMetallicRoughness"]["roughnessFactor"] = true; }},
		{"string metallic", [](Json& doc) { doc["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = "0.5"; }},
		{"fractional texture index", [](Json& doc) { doc["materials"][0]["normalTexture"]["index"] = 0.75; }},
		{"number double sided flag", [](Json& doc) { doc["materials"][0]["doubleSided"] = 1; }},
		{"unknown alpha mode", [](Json& doc) { doc["materials"][0]["alphaMode"] = "TRANSPARENT"; }},
		{"lowercase alpha mode", [](Json& doc) { doc["materials"][0]["alphaMode"] = "mask"; }},
		{"zero accessor count", [](Json& doc) { doc["accessors"][0]["count"] = 0; }},
		{"accessor count mismatch", [](Json& doc) { doc["accessors"][1]["count"] = 2; }},
		{"oversized accessor", [](Json& doc) { doc["accessors"][0]["count"] = std::numeric_limits<uint64_t>::max(); }},
		{"view overflow",
		 [](Json& doc) { doc["bufferViews"][0]["byteOffset"] = std::numeric_limits<uint64_t>::max(); }},
		{"view overrun", [](Json& doc) { doc["bufferViews"][0]["byteLength"] = 1000; }},
		{"accessor overrun", [](Json& doc) { doc["accessors"][0]["byteOffset"] = 143; }},
		{"misaligned accessor", [](Json& doc) { doc["accessors"][0]["byteOffset"] = 1; }},
		{"invalid stride", [](Json& doc) { doc["bufferViews"][0]["byteStride"] = 3; }},
		{"truncated buffer", [](Json& doc) { doc["buffers"][0]["byteLength"] = 151; }},
		{"cyclic nodes", [](Json& doc) { doc["nodes"][0]["children"] = {0}; }},
		{"duplicate scene root", [](Json& doc) { doc["scenes"][0]["nodes"] = {0, 0}; }},
		{"zero quaternion", [](Json& doc) { doc["nodes"][0]["rotation"] = {0, 0, 0, 0}; }},
		{"singular transform", [](Json& doc) { doc["nodes"][0]["scale"] = {0, 1, 1}; }},
		{"missing UV", [](Json& doc) { doc["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_0"); }},
		{"unsupported UV", [](Json& doc) { doc["materials"][0]["normalTexture"]["texCoord"] = 2; }},
		{"negative roughness", [](Json& doc) { doc["materials"][0]["pbrMetallicRoughness"]["roughnessFactor"] = -1; }},
		{"bad alpha", [](Json& doc) { doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] = 2; }},
		{"bad sampler", [](Json& doc) { doc["samplers"][0]["wrapS"] = 17; }},
		{"invalid texture index", [](Json& doc) { doc["materials"][0]["normalTexture"]["index"] = 999; }},
		{"invalid base64", [](Json& doc) { doc["buffers"][0]["uri"] = "data:application/octet-stream;base64,!!!!"; }},
		{"malformed image", [](Json& doc) { doc["images"][0]["uri"] = "data:image/png;base64,AAAA"; }},
		{"unavailable buffer", [](Json& doc) { doc["buffers"][0]["uri"] = "Missing.bin"; }},
		{"URI traversal", [](Json& doc) { doc["buffers"][0]["uri"] = "../../outside.bin"; }},
		{"encoded traversal", [](Json& doc) { doc["buffers"][0]["uri"] = "%2e%2e/%2e%2e/outside.bin"; }},
		{"backslash traversal", [](Json& doc) { doc["buffers"][0]["uri"] = "..\\..\\outside.bin"; }},
		{"URI scheme", [](Json& doc) { doc["buffers"][0]["uri"] = "https://example.com/buffer.bin"; }},
		{"encoded absolute path", [](Json& doc) { doc["buffers"][0]["uri"] = "%2ftmp/file.bin"; }},
		{"null URI", [](Json& doc) { doc["buffers"][0]["uri"] = "file%00.bin"; }},
		{"malformed URI escape", [](Json& doc) { doc["buffers"][0]["uri"] = "file%xx.bin"; }}};
	WriteBytes(temporary.Path / "outside.bin", geometry);
	for (const auto& [name, mutate] : invalidCases)
	{
		auto invalid = fixture;
		mutate(invalid);
		WriteJson(modelPath, invalid);
		Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, name);
	}
	for (const auto value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		auto invalid = fixture;
		auto invalidBuffer = geometry;
		StoreFloat(invalidBuffer, 0, value);
		invalid["buffers"][0]["uri"] = EncodeBuffer(invalidBuffer);
		WriteJson(modelPath, invalid);
		Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, "nonfinite vertex");
	}
	auto invalidIndices = fixture;
	auto invalidIndexBuffer = geometry;
	invalidIndexBuffer[148] = 9;
	invalidIndices["buffers"][0]["uri"] = EncodeBuffer(invalidIndexBuffer);
	WriteJson(modelPath, invalidIndices);
	Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, "out-of-range triangle index");

	// Use a symlink entirely within the test fixture tree that escapes its Project subtree.
	std::error_code symlinkError;
	std::filesystem::create_symlink(temporary.Path / "outside.bin", root / "Data" / "Escape.bin", symlinkError);
	if (!symlinkError)
	{
		auto escaped = fixture;
		escaped["buffers"][0]["uri"] = "../Data/Escape.bin";
		WriteJson(modelPath, escaped);
		Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, "external buffer symlink escape");
		Rejects([&] { (void)importer.LoadImage("Data/Escape.bin"); }, "direct asset symlink escape");
	}
#ifndef _WIN32
	Check(!symlinkError, "symlink containment test must run on Unix");
#endif
	Rejects([&] { (void)importer.ResolvePath("../outside.bin"); }, "direct project escape");
	Rejects([&] { (void)importer.ResolvePath("C:\\Asset.gltf"); }, "drive path escape");
	Rejects([&] { (void)importer.LoadMesh("Missing.gltf"); }, "missing model reported");

	auto fractionalGLB = glbDocument;
	fractionalGLB["accessors"][0]["count"] = 3.75;
	WriteBytes(root / "Meshes" / "Fractional.glb", MakeGLB(fractionalGLB, glbBuffer));
	Rejects([&] { (void)importer.LoadMesh("Meshes/Fractional.glb"); }, "fractional GLB accessor count");
	WriteJson(modelPath, fixture);
	{
		std::ofstream stream(modelPath, std::ios::app);
		stream << " false";
	}
	Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, "trailing glTF JSON data");
	{
		std::ofstream stream(modelPath);
		stream << "{\"extras\":" << std::string(140, '[') << "0" << std::string(140, ']') << "}";
	}
	Rejects([&] { (void)importer.LoadMesh("Meshes/Triangle.gltf"); }, "excessive JSON nesting");

	auto truncated = glb;
	Check(truncated.size() > 16, "GLB truncation fixture requires payload bytes");
	truncated.erase(truncated.end() - 16, truncated.end());
	WriteBytes(root / "Meshes" / "Broken.glb", truncated);
	Rejects([&] { (void)importer.LoadMesh("Meshes/Broken.glb"); }, "truncated GLB");
	std::vector<uint8_t> hdr;
	const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
	hdr.insert(hdr.end(), header.begin(), header.end());
	hdr.insert(hdr.end(), {128, 64, 32, 130, 64, 128, 32, 129});
	WriteBytes(root / "Textures" / "Lighting.hdr", hdr);
	const auto lighting = importer.LoadHDR("Textures/Lighting.hdr");
	Check(lighting.Width == 2 && lighting.Height == 1 && lighting.Pixels.size() == 8, "HDR dimensions decoded");
	Check(lighting.Pixels[0] == 2.0f && lighting.Pixels[1] == 1.0f && lighting.Pixels[2] == 0.5f &&
			  lighting.Pixels[3] == 1.0f,
		  "HDR radiance above one preserved without tonemapping");
	Rejects([&] { (void)importer.LoadImage("Textures/Lighting.hdr"); },
			"HDR cannot silently lose radiance in RGBA8 loader");
	Rejects([&] { (void)importer.LoadHDR("Textures/Color one.png"); }, "LDR image rejected by HDR API");
}
