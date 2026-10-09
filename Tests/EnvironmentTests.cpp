#include "Aster/Renderer/Environment.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string("Environment test failed: ") + message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const char* message)
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

	Aster::HDRImageAsset ConstantImage(uint32_t width, uint32_t height, glm::vec3 radiance)
	{
		Aster::HDRImageAsset image{width, height, std::vector<float>(static_cast<size_t>(width) * height * 4)};
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
		{
			image.Pixels[offset] = radiance.x;
			image.Pixels[offset + 1] = radiance.y;
			image.Pixels[offset + 2] = radiance.z;
			image.Pixels[offset + 3] = 1.0f;
		}
		return image;
	}

	void CheckConstant(const Aster::HDRImageAsset& image, const glm::vec3& expected, float tolerance)
	{
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
		{
			for (size_t component = 0; component < 3; ++component)
			{
				Check(std::abs(image.Pixels[offset + component] - expected[static_cast<int>(component)]) < tolerance,
					  "constant-radiance integration matches analytic result");
			}
			Check(image.Pixels[offset + 3] == 1.0f, "environment output has opaque alpha");
		}
	}

	float Peak(const Aster::HDRImageAsset& image)
	{
		float maximum = 0.0f;
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
		{
			maximum = std::max(maximum, image.Pixels[offset]);
		}
		return maximum;
	}

	double AngularSupport(const Aster::HDRImageAsset& image)
	{
		const float threshold = Peak(image) * 0.1f;
		double bright = 0.0;
		double total = 0.0;
		for (uint32_t y = 0; y < image.Height; ++y)
		{
			const double solidAngle = std::sin((static_cast<double>(y) + 0.5) / image.Height * std::numbers::pi);
			for (uint32_t x = 0; x < image.Width; ++x)
			{
				total += solidAngle;
				if (image.Pixels[(static_cast<size_t>(y) * image.Width + x) * 4] > threshold)
				{
					bright += solidAngle;
				}
			}
		}
		return bright / total;
	}

	void CheckMapsEqual(const Aster::EnvironmentMaps& left, const Aster::EnvironmentMaps& right)
	{
		Check(left.SourceHash == right.SourceHash && left.Settings == right.Settings, "cache metadata round trip");
		Check(left.Sky.Pixels == right.Sky.Pixels && left.Irradiance.Pixels == right.Irradiance.Pixels &&
				  left.BRDFLut.Pixels == right.BRDFLut.Pixels && left.SpecularMips.size() == right.SpecularMips.size(),
			  "cache image data round trip");
		for (size_t mip = 0; mip < left.SpecularMips.size(); ++mip)
		{
			Check(left.SpecularMips[mip].Width == right.SpecularMips[mip].Width &&
					  left.SpecularMips[mip].Height == right.SpecularMips[mip].Height &&
					  left.SpecularMips[mip].Pixels == right.SpecularMips[mip].Pixels,
				  "specular cache mip data round trip");
		}
	}
} // namespace

void RunEnvironmentTests()
{
	using Aster::EnvironmentProcessor;
	Aster::EnvironmentSettings settings{8, 4, 16, 8, 4, 16, 256};
	const auto source = ConstantImage(8, 4, {4.0f, 2.0f, 0.5f});
	const auto environment = EnvironmentProcessor::Build(source, settings);
	Check(environment.Sky.Pixels == source.Pixels, "source sky is preserved");
	CheckConstant(environment.Irradiance, glm::vec3(4.0f, 2.0f, 0.5f) * std::numbers::pi_v<float>, 0.00001f);
	for (const auto& mip : environment.SpecularMips)
	{
		CheckConstant(mip, {4.0f, 2.0f, 0.5f}, 0.00001f);
	}
	Check(environment.SpecularMips[0].Width == 16 && environment.SpecularMips[3].Width == 2 &&
			  environment.SpecularMips[3].Height == 1,
		  "specular mip dimensions halve correctly");
	for (size_t offset = 0; offset < environment.BRDFLut.Pixels.size(); offset += 4)
	{
		const float a = environment.BRDFLut.Pixels[offset];
		const float b = environment.BRDFLut.Pixels[offset + 1];
		Check(std::isfinite(a) && std::isfinite(b) && a >= 0.0f && b >= 0.0f && a + b <= 1.1f,
			  "BRDF split-sum values are finite, positive and energy bounded");
	}
	const size_t nearNormalSmooth = static_cast<size_t>(settings.BRDFSize - 1) * 4;
	const float dielectricReflectance =
		0.04f * environment.BRDFLut.Pixels[nearNormalSmooth] + environment.BRDFLut.Pixels[nearNormalSmooth + 1];
	Check(std::abs(dielectricReflectance - 0.04f) < 0.002f, "smooth dielectric at normal incidence preserves F0");
	Check(environment.BRDFLut.Pixels[1] > environment.BRDFLut.Pixels[nearNormalSmooth + 1],
		  "Fresnel increases at grazing incidence");
	CheckMapsEqual(environment, EnvironmentProcessor::Build(source, settings));

	auto lobe = ConstantImage(64, 32, {0.01f, 0.01f, 0.01f});
	const glm::dvec3 lobeDirection = glm::normalize(glm::dvec3(1.0, 0.1, 0.5));
	for (uint32_t y = 0; y < lobe.Height; ++y)
	{
		for (uint32_t x = 0; x < lobe.Width; ++x)
		{
			const double theta = (static_cast<double>(y) + 0.5) / lobe.Height * std::numbers::pi;
			const double phi = ((static_cast<double>(x) + 0.5) / lobe.Width - 0.5) * 2.0 * std::numbers::pi;
			const glm::dvec3 direction(std::sin(theta) * std::cos(phi), std::cos(theta),
									   std::sin(theta) * std::sin(phi));
			const float intensity =
				static_cast<float>(30.0 * std::exp(80.0 * (glm::dot(direction, lobeDirection) - 1.0)) + 0.01);
			const size_t offset = (static_cast<size_t>(y) * lobe.Width + x) * 4;
			lobe.Pixels[offset] = lobe.Pixels[offset + 1] = lobe.Pixels[offset + 2] = intensity;
		}
	}
	auto lobeSettings = settings;
	lobeSettings.SpecularWidth = 64;
	lobeSettings.SpecularHeight = 32;
	lobeSettings.SampleCount = 512;
	const auto filtered = EnvironmentProcessor::Build(lobe, lobeSettings);
	Check(Peak(filtered.SpecularMips.front()) > Peak(filtered.SpecularMips.back()) * 3.0f,
		  "roughness reduces directional radiance peak");
	Check(AngularSupport(filtered.SpecularMips.back()) > AngularSupport(filtered.SpecularMips.front()) * 3.0,
		  "GGX roughness broadens the angular lobe");
	float minimumIrradiance = std::numeric_limits<float>::max();
	for (size_t offset = 0; offset < filtered.Irradiance.Pixels.size(); offset += 4)
	{
		minimumIrradiance = std::min(minimumIrradiance, filtered.Irradiance.Pixels[offset]);
	}
	Check(Peak(filtered.Irradiance) > minimumIrradiance * 3.0f, "irradiance convolution retains lighting direction");

	auto changed = source;
	changed.Pixels[0] += 1.0f;
	Check(EnvironmentProcessor::Fingerprint(source, settings) != EnvironmentProcessor::Fingerprint(changed, settings),
		  "cache key changes with source pixels");
	auto differentSettings = settings;
	differentSettings.SampleCount = 512;
	Check(EnvironmentProcessor::Fingerprint(source, settings) !=
			  EnvironmentProcessor::Fingerprint(source, differentSettings),
		  "cache key changes with integration settings");
	for (const float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		auto malformed = source;
		malformed.Pixels[0] = invalid;
		Rejects([&] { (void)EnvironmentProcessor::Build(malformed, settings); }, "invalid source radiance rejected");
	}
	auto malformed = source;
	malformed.Pixels.pop_back();
	Rejects([&] { (void)EnvironmentProcessor::Build(malformed, settings); }, "pixel count mismatch rejected");
	Rejects([&] { (void)EnvironmentProcessor::Build({}, settings); }, "empty environment rejected");
	auto invalidSettings = settings;
	invalidSettings.SampleCount = 0;
	Rejects([&] { (void)EnvironmentProcessor::Build(source, invalidSettings); }, "zero samples rejected");
	invalidSettings = settings;
	invalidSettings.SpecularMipCount = 99;
	Rejects([&] { (void)EnvironmentProcessor::Build(source, invalidSettings); }, "excessive mip count rejected");
	invalidSettings = settings;
	invalidSettings.SpecularMipCount = 1;
	Rejects([&] { (void)EnvironmentProcessor::Build(source, invalidSettings); }, "single roughness mip rejected");
	invalidSettings = settings;
	invalidSettings.BRDFSize = 0;
	Rejects([&] { (void)EnvironmentProcessor::Build(source, invalidSettings); }, "zero LUT size rejected");

	std::random_device random;
	const auto directory = std::filesystem::current_path() / ("EnvironmentTestArtifacts-" + std::to_string(random()));
	std::filesystem::create_directory(directory);
	const auto cachePath = directory / "Environment.asteribl";
	try
	{
		EnvironmentProcessor::SaveCache(environment, cachePath);
		CheckMapsEqual(environment, EnvironmentProcessor::LoadCache(cachePath));
		CheckMapsEqual(environment, EnvironmentProcessor::BuildCached(source, cachePath, settings));
		const auto rebuilt = EnvironmentProcessor::BuildCached(changed, cachePath, settings);
		Check(rebuilt.SourceHash != environment.SourceHash && rebuilt.Sky.Pixels == changed.Pixels,
			  "stale cached environment is rebuilt");
		CheckMapsEqual(rebuilt, EnvironmentProcessor::LoadCache(cachePath));
		std::ofstream(cachePath, std::ios::binary | std::ios::app).put('x');
		Rejects([&] { (void)EnvironmentProcessor::LoadCache(cachePath); }, "trailing cache data rejected");
		EnvironmentProcessor::SaveCache(environment, cachePath);
		{
			std::fstream cache(cachePath, std::ios::binary | std::ios::in | std::ios::out);
			cache.seekp(-16, std::ios::end);
			cache.put('\1');
		}
		Rejects([&] { (void)EnvironmentProcessor::LoadCache(cachePath); },
				"derived texture corruption rejected by checksum");
		EnvironmentProcessor::SaveCache(environment, cachePath);
		{
			std::fstream cache(cachePath, std::ios::binary | std::ios::in | std::ios::out);
			cache.seekp(8);
			cache.put('\2');
		}
		Rejects([&] { (void)EnvironmentProcessor::LoadCache(cachePath); }, "unsupported cache version rejected");
		std::filesystem::resize_file(cachePath, 56);
		Rejects([&] { (void)EnvironmentProcessor::LoadCache(cachePath); }, "truncated cache rejected");
		std::filesystem::remove_all(directory);
	}
	catch (...)
	{
		std::filesystem::remove_all(directory);
		throw;
	}

	Aster::AssetImporter importer(std::filesystem::path(ASTER_SOURCE_DIR) / "Assets");
	const auto studio = importer.LoadHDR("Environment/StudioSmall09.hdr");
	Check(studio.Width == 1024 && studio.Height == 512 && Peak(studio) > 1.0f,
		  "real Poly Haven HDRI has expected resolution and HDR radiance");
	const auto studioEnvironment = EnvironmentProcessor::Build(studio, settings);
	Check(Peak(studioEnvironment.Irradiance) > 0.0f && Peak(studioEnvironment.SpecularMips[0]) > 0.0f,
		  "real HDRI produces lighting maps");
}
