#pragma once

#include "Aster/Assets/AssetImporter.h"

#include <filesystem>

namespace Aster
{
	struct EnvironmentSettings
	{
		uint32_t IrradianceWidth = 32;
		uint32_t IrradianceHeight = 16;
		uint32_t SpecularWidth = 128;
		uint32_t SpecularHeight = 64;
		uint32_t SpecularMipCount = 6;
		uint32_t BRDFSize = 64;
		uint32_t SampleCount = 256;
		bool operator==(const EnvironmentSettings&) const = default;
	};

	struct EnvironmentMaps
	{
		HDRImageAsset Sky;
		// Cosine-weighted hemispherical integral. Lambert shading multiplies by albedo / pi.
		HDRImageAsset Irradiance;
		// GGX perceptual roughness is mip / (mipCount - 1), with alpha = roughness squared.
		std::vector<HDRImageAsset> SpecularMips;
		// RG = split-sum A, B; X = N dot V, Y = perceptual roughness; specular = F0 * A + B.
		HDRImageAsset BRDFLut;
		EnvironmentSettings Settings;
		uint64_t SourceHash = 0;
	};

	class EnvironmentProcessor
	{
	  public:
		// Equirectangular convention: +Y up, u = atan2(z, x)/(2*pi)+0.5, v = acos(y)/pi.
		// Sampling uses U repeat and V clamp. Source pixels are linear RGBA32F.
		[[nodiscard]] static EnvironmentMaps Build(const HDRImageAsset& source,
												   const EnvironmentSettings& settings = {});
		[[nodiscard]] static uint64_t Fingerprint(const HDRImageAsset& source,
												  const EnvironmentSettings& settings = {});
		static void SaveCache(const EnvironmentMaps& environment, const std::filesystem::path& path);
		[[nodiscard]] static EnvironmentMaps LoadCache(const std::filesystem::path& path);
		[[nodiscard]] static EnvironmentMaps BuildCached(const HDRImageAsset& source, const std::filesystem::path& path,
														 const EnvironmentSettings& settings = {});
	};
} // namespace Aster
