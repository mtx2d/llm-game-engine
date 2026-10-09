#include "Aster/Renderer/Environment.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Aster
{
	namespace
	{
		constexpr double s_Pi = std::numbers::pi;
		constexpr uint64_t s_MaxCacheBytes = 512ULL * 1024ULL * 1024ULL;
		constexpr uint32_t s_CacheVersion = 1;

		void Require(bool condition, const char* message)
		{
			if (!condition)
			{
				throw std::invalid_argument(message);
			}
		}

		void ValidateImage(const HDRImageAsset& image)
		{
			const uint64_t values = static_cast<uint64_t>(image.Width) * image.Height * 4;
			Require(image.Width > 0 && image.Height > 0 && image.Width <= 16384 && image.Height <= 16384 &&
						values <= s_MaxCacheBytes / sizeof(float) && values == image.Pixels.size(),
					"Invalid environment image dimensions or pixel count");
			for (const auto value : image.Pixels)
			{
				Require(std::isfinite(value) && value >= 0.0f, "Environment pixels must be finite and nonnegative");
			}
		}

		void ValidateSettings(const EnvironmentSettings& settings)
		{
			Require(settings.IrradianceWidth > 0 && settings.IrradianceHeight > 0 && settings.IrradianceWidth <= 1024 &&
						settings.IrradianceHeight <= 1024,
					"Invalid irradiance dimensions");
			Require(settings.SpecularWidth > 0 && settings.SpecularHeight > 0 && settings.SpecularWidth <= 4096 &&
						settings.SpecularHeight <= 4096,
					"Invalid specular dimensions");
			Require(settings.SpecularMipCount >= 2 &&
						settings.SpecularMipCount <= static_cast<uint32_t>(std::bit_width(
														 std::max(settings.SpecularWidth, settings.SpecularHeight))),
					"Specular mip count must be between two and the complete mip-chain size");
			Require(settings.BRDFSize > 0 && settings.BRDFSize <= 1024, "Invalid BRDF lookup dimensions");
			Require(settings.SampleCount >= 16 && settings.SampleCount <= 65536,
					"Environment sample count must be in [16, 65536]");
			const uint64_t work = (static_cast<uint64_t>(settings.IrradianceWidth) * settings.IrradianceHeight +
								   static_cast<uint64_t>(settings.SpecularWidth) * settings.SpecularHeight * 2 +
								   static_cast<uint64_t>(settings.BRDFSize) * settings.BRDFSize) *
								  settings.SampleCount;
			Require(work <= 2000000000ULL, "Environment preprocessing exceeds the configured work limit");
		}

		HDRImageAsset MakeImage(uint32_t width, uint32_t height)
		{
			return {width, height, std::vector<float>(static_cast<size_t>(width) * height * 4, 0.0f)};
		}

		void Store(HDRImageAsset& image, uint32_t x, uint32_t y, const glm::dvec3& value)
		{
			const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
			for (size_t component = 0; component < 3; ++component)
			{
				const double channel = value[static_cast<int>(component)];
				Require(std::isfinite(channel) && channel >= 0.0 && channel <= std::numeric_limits<float>::max(),
						"Environment integration exceeds finite float range");
				image.Pixels[offset + component] = static_cast<float>(channel);
			}
			image.Pixels[offset + 3] = 1.0f;
		}

		glm::dvec3 Direction(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			const double phi = ((static_cast<double>(x) + 0.5) / width - 0.5) * 2.0 * s_Pi;
			const double theta = (static_cast<double>(y) + 0.5) / height * s_Pi;
			return {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
		}

		glm::dvec3 Sample(const HDRImageAsset& image, const glm::dvec3& direction)
		{
			const double u = std::atan2(direction.z, direction.x) / (2.0 * s_Pi) + 0.5;
			const double v = std::acos(std::clamp(direction.y, -1.0, 1.0)) / s_Pi;
			const double pixelX = u * image.Width - 0.5;
			const double pixelY = v * image.Height - 0.5;
			const auto x0 = static_cast<int>(std::floor(pixelX));
			const auto y0 = static_cast<int>(std::floor(pixelY));
			const double fractionX = pixelX - x0;
			const double fractionY = pixelY - y0;
			auto read = [&](int x, int y)
			{
				const auto width = static_cast<int>(image.Width);
				x = (x % width + width) % width;
				y = std::clamp(y, 0, static_cast<int>(image.Height) - 1);
				const size_t offset = (static_cast<size_t>(y) * image.Width + static_cast<size_t>(x)) * 4;
				return glm::dvec3(image.Pixels[offset], image.Pixels[offset + 1], image.Pixels[offset + 2]);
			};
			return glm::mix(glm::mix(read(x0, y0), read(x0 + 1, y0), fractionX),
							glm::mix(read(x0, y0 + 1), read(x0 + 1, y0 + 1), fractionX), fractionY);
		}

		double RadicalInverse(uint32_t bits)
		{
			bits = (bits << 16) | (bits >> 16);
			bits = ((bits & 0x55555555U) << 1) | ((bits & 0xaaaaaaaaU) >> 1);
			bits = ((bits & 0x33333333U) << 2) | ((bits & 0xccccccccU) >> 2);
			bits = ((bits & 0x0f0f0f0fU) << 4) | ((bits & 0xf0f0f0f0U) >> 4);
			bits = ((bits & 0x00ff00ffU) << 8) | ((bits & 0xff00ff00U) >> 8);
			return static_cast<double>(bits) / 4294967296.0;
		}

		glm::dvec3 ImportanceGGX(uint32_t index, uint32_t count, double roughness)
		{
			// Perceptual roughness parameterization and split-sum importance sampling:
			// Brian Karis, Real Shading in Unreal Engine 4 (SIGGRAPH 2013), Epic Games.
			// https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf
			const double alpha = roughness * roughness;
			const double phi = 2.0 * s_Pi * (static_cast<double>(index) + 0.5) / count;
			const double xi = RadicalInverse(index);
			const double cosine = std::sqrt((1.0 - xi) / (1.0 + (alpha * alpha - 1.0) * xi));
			const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
			return {std::cos(phi) * sine, std::sin(phi) * sine, cosine};
		}

		glm::dmat3 Basis(const glm::dvec3& normal)
		{
			const auto up = std::abs(normal.z) < 0.999 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
			const auto tangent = glm::normalize(glm::cross(up, normal));
			return {tangent, glm::cross(normal, tangent), normal};
		}

		double SmithG1(double cosine, double alphaSquared)
		{
			// Exact isotropic Smith masking for GGX; no direct-light roughness remap.
			return 2.0 * cosine / (cosine + std::sqrt(alphaSquared + (1.0 - alphaSquared) * cosine * cosine));
		}

		void HashWord(uint64_t& hash, uint32_t word)
		{
			for (int byte = 0; byte < 4; ++byte)
			{
				hash ^= (word >> (byte * 8)) & 0xffU;
				hash *= 1099511628211ULL;
			}
		}

		std::array<uint32_t, 7> SettingsWords(const EnvironmentSettings& settings)
		{
			return {settings.IrradianceWidth, settings.IrradianceHeight, settings.SpecularWidth,
					settings.SpecularHeight,  settings.SpecularMipCount, settings.BRDFSize,
					settings.SampleCount};
		}

		uint64_t CacheChecksum(const EnvironmentMaps& environment)
		{
			uint64_t hash = 14695981039346656037ULL;
			auto addImage = [&](const HDRImageAsset& image)
			{
				HashWord(hash, image.Width);
				HashWord(hash, image.Height);
				for (const auto value : image.Pixels)
				{
					HashWord(hash, std::bit_cast<uint32_t>(value));
				}
			};
			HashWord(hash, s_CacheVersion);
			HashWord(hash, static_cast<uint32_t>(environment.SourceHash));
			HashWord(hash, static_cast<uint32_t>(environment.SourceHash >> 32));
			for (const auto setting : SettingsWords(environment.Settings))
			{
				HashWord(hash, setting);
			}
			addImage(environment.Sky);
			addImage(environment.Irradiance);
			for (const auto& mip : environment.SpecularMips)
			{
				addImage(mip);
			}
			addImage(environment.BRDFLut);
			return hash;
		}

		void ValidateMaps(const EnvironmentMaps& environment)
		{
			ValidateSettings(environment.Settings);
			ValidateImage(environment.Sky);
			ValidateImage(environment.Irradiance);
			ValidateImage(environment.BRDFLut);
			Require(environment.Irradiance.Width == environment.Settings.IrradianceWidth &&
						environment.Irradiance.Height == environment.Settings.IrradianceHeight,
					"Irradiance dimensions do not match settings");
			Require(environment.BRDFLut.Width == environment.Settings.BRDFSize &&
						environment.BRDFLut.Height == environment.Settings.BRDFSize,
					"BRDF LUT dimensions do not match settings");
			Require(environment.SpecularMips.size() == environment.Settings.SpecularMipCount,
					"Specular mip count does not match settings");
			uint32_t width = environment.Settings.SpecularWidth;
			uint32_t height = environment.Settings.SpecularHeight;
			uint64_t values = environment.Sky.Pixels.size() + environment.Irradiance.Pixels.size() +
							  environment.BRDFLut.Pixels.size();
			for (const auto& mip : environment.SpecularMips)
			{
				ValidateImage(mip);
				Require(mip.Width == width && mip.Height == height, "Specular mip dimensions are invalid");
				width = std::max(1U, width / 2);
				height = std::max(1U, height / 2);
				values += mip.Pixels.size();
			}
			Require(values * sizeof(float) < s_MaxCacheBytes - 4096, "Environment cache exceeds memory limit");
		}

		void WriteWord(std::ostream& output, uint32_t word)
		{
			std::array<char, 4> bytes{};
			for (size_t index = 0; index < bytes.size(); ++index)
			{
				bytes[index] = static_cast<char>(word >> (index * 8));
			}
			output.write(bytes.data(), bytes.size());
		}

		uint32_t ReadWord(std::istream& input)
		{
			std::array<uint8_t, 4> bytes{};
			input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
			return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
				   (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
		}

		void WriteImage(std::ostream& output, const HDRImageAsset& image)
		{
			WriteWord(output, image.Width);
			WriteWord(output, image.Height);
			for (const auto pixel : image.Pixels)
			{
				WriteWord(output, std::bit_cast<uint32_t>(pixel));
			}
		}

		HDRImageAsset ReadImage(std::istream& input, uint64_t& remaining)
		{
			Require(remaining >= 8, "Truncated environment image header");
			const auto width = ReadWord(input);
			const auto height = ReadWord(input);
			remaining -= 8;
			const uint64_t bytes = static_cast<uint64_t>(width) * height * 4 * sizeof(float);
			Require(width > 0 && width <= 16384 && height > 0 && height <= 16384 && bytes <= remaining,
					"Invalid environment cache image size");
			auto image = MakeImage(width, height);
			for (auto& pixel : image.Pixels)
			{
				pixel = std::bit_cast<float>(ReadWord(input));
			}
			remaining -= bytes;
			ValidateImage(image);
			return image;
		}
	} // namespace

	uint64_t EnvironmentProcessor::Fingerprint(const HDRImageAsset& source, const EnvironmentSettings& settings)
	{
		ValidateImage(source);
		ValidateSettings(settings);
		uint64_t hash = 14695981039346656037ULL;
		HashWord(hash, s_CacheVersion);
		HashWord(hash, source.Width);
		HashWord(hash, source.Height);
		for (const auto setting : SettingsWords(settings))
		{
			HashWord(hash, setting);
		}
		for (const auto pixel : source.Pixels)
		{
			HashWord(hash, std::bit_cast<uint32_t>(pixel));
		}
		return hash;
	}

	EnvironmentMaps EnvironmentProcessor::Build(const HDRImageAsset& source, const EnvironmentSettings& settings)
	{
		EnvironmentMaps environment;
		environment.SourceHash = Fingerprint(source, settings);
		environment.Settings = settings;
		environment.Sky = source;
		environment.Irradiance = MakeImage(settings.IrradianceWidth, settings.IrradianceHeight);
		std::vector<glm::dvec3> cosineSamples;
		cosineSamples.reserve(settings.SampleCount);
		for (uint32_t index = 0; index < settings.SampleCount; ++index)
		{
			const double radialSquared = (static_cast<double>(index) + 0.5) / settings.SampleCount;
			const double phi = 2.0 * s_Pi * RadicalInverse(index);
			cosineSamples.emplace_back(std::sqrt(radialSquared) * std::cos(phi),
									   std::sqrt(radialSquared) * std::sin(phi), std::sqrt(1.0 - radialSquared));
		}
		for (uint32_t y = 0; y < settings.IrradianceHeight; ++y)
		{
			for (uint32_t x = 0; x < settings.IrradianceWidth; ++x)
			{
				const auto basis = Basis(Direction(x, y, settings.IrradianceWidth, settings.IrradianceHeight));
				glm::dvec3 irradiance(0.0);
				for (const auto& direction : cosineSamples)
				{
					irradiance += Sample(source, basis * direction);
				}
				Store(environment.Irradiance, x, y, irradiance * (s_Pi / settings.SampleCount));
			}
		}
		uint32_t width = settings.SpecularWidth;
		uint32_t height = settings.SpecularHeight;
		for (uint32_t mip = 0; mip < settings.SpecularMipCount; ++mip)
		{
			const double roughness = static_cast<double>(mip) / (settings.SpecularMipCount - 1);
			auto image = MakeImage(width, height);
			std::vector<glm::dvec3> halfSamples;
			halfSamples.reserve(settings.SampleCount);
			for (uint32_t index = 0; index < settings.SampleCount; ++index)
			{
				halfSamples.push_back(ImportanceGGX(index, settings.SampleCount, roughness));
			}
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const auto normal = Direction(x, y, width, height);
					if (mip == 0)
					{
						Store(image, x, y, Sample(source, normal));
						continue;
					}
					const auto basis = Basis(normal);
					glm::dvec3 radiance(0.0);
					double weight = 0.0;
					for (const auto& localHalf : halfSamples)
					{
						const auto halfway = basis * localHalf;
						const auto light = 2.0 * glm::dot(normal, halfway) * halfway - normal;
						const double cosine = std::max(0.0, glm::dot(normal, light));
						if (cosine > 0.0)
						{
							radiance += Sample(source, light) * cosine;
							weight += cosine;
						}
					}
					Require(weight > 0.0, "GGX sampling produced no valid directions");
					Store(image, x, y, radiance / weight);
				}
			}
			environment.SpecularMips.push_back(std::move(image));
			width = std::max(1U, width / 2);
			height = std::max(1U, height / 2);
		}
		environment.BRDFLut = MakeImage(settings.BRDFSize, settings.BRDFSize);
		for (uint32_t y = 0; y < settings.BRDFSize; ++y)
		{
			const double roughness = (static_cast<double>(y) + 0.5) / settings.BRDFSize;
			const double alphaSquared = roughness * roughness * roughness * roughness;
			std::vector<glm::dvec3> halfSamples;
			halfSamples.reserve(settings.SampleCount);
			for (uint32_t index = 0; index < settings.SampleCount; ++index)
			{
				halfSamples.push_back(ImportanceGGX(index, settings.SampleCount, roughness));
			}
			for (uint32_t x = 0; x < settings.BRDFSize; ++x)
			{
				const double nDotV = (static_cast<double>(x) + 0.5) / settings.BRDFSize;
				const glm::dvec3 view(std::sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
				glm::dvec3 integrated(0.0);
				for (const auto& halfway : halfSamples)
				{
					const double vDotH = std::max(0.0, glm::dot(view, halfway));
					const auto light = 2.0 * vDotH * halfway - view;
					if (light.z > 0.0 && halfway.z > 0.0)
					{
						const double geometry = SmithG1(nDotV, alphaSquared) * SmithG1(light.z, alphaSquared);
						const double visibility = geometry * vDotH / (halfway.z * nDotV);
						const double fresnel = std::pow(1.0 - vDotH, 5.0);
						integrated.x += (1.0 - fresnel) * visibility;
						integrated.y += fresnel * visibility;
					}
				}
				Store(environment.BRDFLut, x, y, integrated / static_cast<double>(settings.SampleCount));
			}
		}
		ValidateMaps(environment);
		return environment;
	}

	void EnvironmentProcessor::SaveCache(const EnvironmentMaps& environment, const std::filesystem::path& path)
	{
		ValidateMaps(environment);
		Require(environment.SourceHash == Fingerprint(environment.Sky, environment.Settings),
				"Environment cache source fingerprint is inconsistent");
		std::random_device random;
		auto temporary = path;
		temporary += ".tmp-" + std::to_string(random()) + "-" + std::to_string(random());
		try
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output.exceptions(std::ios::badbit | std::ios::failbit);
			output.write("ASTERIBL", 8);
			WriteWord(output, s_CacheVersion);
			WriteWord(output, static_cast<uint32_t>(environment.SourceHash));
			WriteWord(output, static_cast<uint32_t>(environment.SourceHash >> 32));
			for (const auto setting : SettingsWords(environment.Settings))
			{
				WriteWord(output, setting);
			}
			WriteImage(output, environment.Sky);
			WriteImage(output, environment.Irradiance);
			for (const auto& image : environment.SpecularMips)
			{
				WriteImage(output, image);
			}
			WriteImage(output, environment.BRDFLut);
			const auto checksum = CacheChecksum(environment);
			WriteWord(output, static_cast<uint32_t>(checksum));
			WriteWord(output, static_cast<uint32_t>(checksum >> 32));
			output.flush();
			output.close();
#ifdef _WIN32
			if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
										"Cannot replace environment cache");
			}
#else
			std::filesystem::rename(temporary, path);
#endif
		}
		catch (...)
		{
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			throw;
		}
	}

	EnvironmentMaps EnvironmentProcessor::LoadCache(const std::filesystem::path& path)
	{
		uint64_t remaining = std::filesystem::file_size(path);
		Require(remaining >= 56 && remaining < s_MaxCacheBytes, "Invalid environment cache file size");
		std::ifstream input(path, std::ios::binary);
		input.exceptions(std::ios::badbit | std::ios::failbit);
		std::array<char, 8> magic;
		input.read(magic.data(), magic.size());
		Require(magic == std::array<char, 8>{'A', 'S', 'T', 'E', 'R', 'I', 'B', 'L'},
				"Invalid environment cache signature");
		Require(ReadWord(input) == s_CacheVersion, "Unsupported environment cache version");
		EnvironmentMaps environment;
		environment.SourceHash = ReadWord(input);
		environment.SourceHash |= static_cast<uint64_t>(ReadWord(input)) << 32;
		environment.Settings = {ReadWord(input), ReadWord(input), ReadWord(input), ReadWord(input),
								ReadWord(input), ReadWord(input), ReadWord(input)};
		ValidateSettings(environment.Settings);
		remaining -= 48;
		environment.Sky = ReadImage(input, remaining);
		environment.Irradiance = ReadImage(input, remaining);
		for (uint32_t index = 0; index < environment.Settings.SpecularMipCount; ++index)
		{
			environment.SpecularMips.push_back(ReadImage(input, remaining));
		}
		environment.BRDFLut = ReadImage(input, remaining);
		Require(remaining == 8, "Unexpected trailing or truncated environment cache data");
		uint64_t checksum = ReadWord(input);
		checksum |= static_cast<uint64_t>(ReadWord(input)) << 32;
		ValidateMaps(environment);
		Require(checksum == CacheChecksum(environment), "Environment cache checksum does not match");
		Require(environment.SourceHash == Fingerprint(environment.Sky, environment.Settings),
				"Environment cache source fingerprint does not match");
		return environment;
	}

	EnvironmentMaps EnvironmentProcessor::BuildCached(const HDRImageAsset& source, const std::filesystem::path& path,
													  const EnvironmentSettings& settings)
	{
		const auto expected = Fingerprint(source, settings);
		if (std::filesystem::exists(path))
		{
			auto cached = LoadCache(path);
			if (cached.SourceHash == expected)
			{
				return cached;
			}
		}
		auto environment = Build(source, settings);
		SaveCache(environment, path);
		return environment;
	}
} // namespace Aster
