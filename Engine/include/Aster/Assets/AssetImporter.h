#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Aster
{
	struct ImageAsset
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// RGBA8 in file color space. Color conversion is chosen per material usage.
		std::vector<uint8_t> Pixels;
	};

	struct HDRImageAsset
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Linear RGBA32F; radiance above 1 is preserved.
		std::vector<float> Pixels;
	};

	struct TextureAsset
	{
		std::string Name;
		ImageAsset Image;
		// glTF/OpenGL sampler constants, preserved for renderer translation.
		int MinFilter = 9987;
		int MagFilter = 9729;
		int WrapU = 10497;
		int WrapV = 10497;
	};

	struct TextureReference
	{
		int32_t Texture = -1;
		uint32_t TexCoord = 0;
		bool SRGB = false;
		// Normal scale or occlusion strength; one for other usages.
		float Scale = 1.0f;
		glm::mat3 Transform{1.0f};
	};

	enum class MaterialAlphaMode
	{
		Opaque,
		Mask,
		Blend
	};

	struct MaterialAsset
	{
		std::string Name;
		glm::vec4 BaseColorFactor{1.0f};
		float MetallicFactor = 1.0f;
		float RoughnessFactor = 1.0f;
		glm::vec3 EmissiveFactor{0.0f};
		TextureReference BaseColorTexture;
		TextureReference MetallicRoughnessTexture;
		TextureReference NormalTexture;
		TextureReference OcclusionTexture;
		TextureReference EmissiveTexture;
		MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		bool Unlit = false;
	};

	struct MeshVertex
	{
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f, 0.0f, 1.0f};
		glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		glm::vec2 TexCoord{0.0f};
		glm::vec2 TexCoord1{0.0f};
		glm::vec4 Color{1.0f};
	};

	struct MeshPrimitive
	{
		std::string NodeName;
		std::vector<MeshVertex> Vertices;
		std::vector<uint32_t> Indices;
		uint32_t MaterialIndex = 0;
		// Render with entityWorld * Transform. Vertices remain in mesh local space.
		glm::mat4 Transform{1.0f};
	};

	struct MeshAsset
	{
		std::vector<MeshPrimitive> Primitives;
		std::vector<MaterialAsset> Materials;
		std::vector<TextureAsset> Textures;
		// Project-relative input files needed by an exported game, including source.
		std::vector<std::filesystem::path> Dependencies;
	};

	class AssetImporter
	{
	  public:
		explicit AssetImporter(std::filesystem::path projectRoot);
		[[nodiscard]] const std::filesystem::path& GetRoot() const noexcept
		{
			return m_ProjectRoot;
		}
		[[nodiscard]] MeshAsset LoadMesh(const std::filesystem::path& relativePath) const;
		[[nodiscard]] ImageAsset LoadImage(const std::filesystem::path& relativePath) const;
		[[nodiscard]] HDRImageAsset LoadHDR(const std::filesystem::path& relativePath) const;
		[[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path& relativePath) const;

	  private:
		std::filesystem::path m_ProjectRoot;
	};
} // namespace Aster
