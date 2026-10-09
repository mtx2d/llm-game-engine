#include "AssetIO.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace Aster::AssetDetail
{
	namespace
	{
		int HexDigit(char value)
		{
			if (value >= '0' && value <= '9')
			{
				return value - '0';
			}
			if (value >= 'a' && value <= 'f')
			{
				return value - 'a' + 10;
			}
			if (value >= 'A' && value <= 'F')
			{
				return value - 'A' + 10;
			}
			throw std::invalid_argument("Malformed percent encoding in glTF URI");
		}

		std::string DecodeURI(const std::string& uri)
		{
			std::string decoded;
			decoded.reserve(uri.size());
			for (size_t index = 0; index < uri.size(); ++index)
			{
				if (uri[index] == '%')
				{
					if (index + 2 >= uri.size())
					{
						throw std::invalid_argument("Truncated percent encoding in glTF URI");
					}
					decoded.push_back(static_cast<char>((HexDigit(uri[index + 1]) << 4) | HexDigit(uri[index + 2])));
					index += 2;
				}
				else
				{
					decoded.push_back(uri[index]);
				}
			}
			return decoded;
		}

		std::vector<uint8_t> DecodeBase64(std::string_view encoded)
		{
			if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() / 4 > s_MaxAssetBytes / 3)
			{
				throw std::invalid_argument("Invalid or oversized base64 data URI");
			}
			const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::vector<uint8_t> decoded;
			decoded.reserve(encoded.size() / 4 * 3);
			for (size_t offset = 0; offset < encoded.size(); offset += 4)
			{
				uint32_t word = 0;
				size_t padding = 0;
				for (size_t index = 0; index < 4; ++index)
				{
					const char character = encoded[offset + index];
					if (character == '=')
					{
						if (index < 2 || offset + 4 != encoded.size())
						{
							throw std::invalid_argument("Misplaced base64 padding");
						}
						++padding;
						word <<= 6;
					}
					else
					{
						const auto digit = alphabet.find(character);
						if (digit == std::string_view::npos || padding != 0)
						{
							throw std::invalid_argument("Invalid base64 character or padding");
						}
						word = (word << 6) | static_cast<uint32_t>(digit);
					}
				}
				if ((padding == 1 && (word & 0xffU) != 0) || (padding == 2 && (word & 0xffffU) != 0))
				{
					throw std::invalid_argument("Nonzero base64 padding bits");
				}
				decoded.push_back(static_cast<uint8_t>(word >> 16));
				if (padding < 2)
				{
					decoded.push_back(static_cast<uint8_t>(word >> 8));
				}
				if (padding == 0)
				{
					decoded.push_back(static_cast<uint8_t>(word));
				}
			}
			return decoded;
		}

		void ValidateDimensions(int width, int height, size_t elementSize)
		{
			if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
				static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4 * elementSize > s_MaxDecodedBytes)
			{
				throw std::invalid_argument("Image exceeds supported dimensions or decoded memory limit");
			}
		}
	} // namespace

	std::filesystem::path ResolveContained(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		std::string portable = relative.generic_string();
		if (portable.empty() || portable.size() > 4096 || portable.find('\0') != std::string::npos ||
			portable.find(':') != std::string::npos)
		{
			throw std::invalid_argument("Asset path must be a relative project path without a URI scheme");
		}
		std::replace(portable.begin(), portable.end(), '\\', '/');
		const std::filesystem::path normalized(portable);
		if (normalized.is_absolute() || normalized.has_root_name())
		{
			throw std::invalid_argument("Absolute asset paths are not supported");
		}
		const auto resolved = std::filesystem::weakly_canonical(root / normalized);
		auto candidate = resolved.begin();
		for (const auto& segment : root)
		{
			if (candidate == resolved.end() || *candidate != segment)
			{
				throw std::invalid_argument("Asset path escapes the project root");
			}
			++candidate;
		}
		if (candidate == resolved.end())
		{
			throw std::invalid_argument("Asset path must name a file within the project");
		}
		return resolved;
	}

	std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
		{
			throw std::runtime_error("Cannot open asset: " + path.string());
		}
		const auto size = file.tellg();
		if (size <= 0 || static_cast<uint64_t>(size) > s_MaxAssetBytes)
		{
			throw std::invalid_argument("Asset is empty or exceeds the 256 MiB file limit: " + path.string());
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		{
			throw std::runtime_error("Cannot read complete asset: " + path.string());
		}
		return bytes;
	}

	std::vector<uint8_t> ReadURI(const std::filesystem::path& root, const std::filesystem::path& source,
								 const std::string& uri, std::vector<std::filesystem::path>& dependencies)
	{
		if (uri.starts_with("data:"))
		{
			const auto marker = uri.find(";base64,");
			if (marker == std::string::npos)
			{
				throw std::invalid_argument("Only base64 data URIs are supported");
			}
			return DecodeBase64(std::string_view(uri).substr(marker + 8));
		}
		const auto decoded = DecodeURI(uri);
		// Validate before joining, since an absolute RHS would discard the source directory.
		std::string portable = decoded;
		std::replace(portable.begin(), portable.end(), '\\', '/');
		if (portable.empty() || portable.front() == '/' || portable.find(':') != std::string::npos ||
			portable.find('\0') != std::string::npos || portable.find_first_of("?#") != std::string::npos)
		{
			throw std::invalid_argument("External glTF URI must name a relative local file");
		}
		const auto relative = source.parent_path().lexically_relative(root) / portable;
		const auto path = ResolveContained(root, relative);
		const auto dependency = path.lexically_relative(root);
		if (std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end())
		{
			dependencies.push_back(dependency);
		}
		return ReadBytes(path);
	}

	ImageAsset DecodeImage(std::span<const uint8_t> bytes)
	{
		if (bytes.empty() || bytes.size() > s_MaxAssetBytes)
		{
			throw std::invalid_argument("Image payload is empty or too large");
		}
		int width = 0;
		int height = 0;
		int channels = 0;
		if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels))
		{
			throw std::invalid_argument("Invalid image header");
		}
		ValidateDimensions(width, height, sizeof(uint8_t));
		if (stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size())))
		{
			throw std::invalid_argument("HDR images require LoadHDR to preserve radiance");
		}
		std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
			stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels,
								  STBI_rgb_alpha),
			stbi_image_free);
		if (!pixels)
		{
			throw std::invalid_argument("Image decoding failed");
		}
		const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		return {static_cast<uint32_t>(width), static_cast<uint32_t>(height), {pixels.get(), pixels.get() + pixelBytes}};
	}

	HDRImageAsset DecodeHDR(std::span<const uint8_t> bytes)
	{
		int width = 0;
		int height = 0;
		int channels = 0;
		if (!stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size())) ||
			!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels))
		{
			throw std::invalid_argument("Expected a Radiance HDR image");
		}
		ValidateDimensions(width, height, sizeof(float));
		std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
			stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels,
								   STBI_rgb_alpha),
			stbi_image_free);
		if (!pixels)
		{
			throw std::invalid_argument("HDR decoding failed");
		}
		const size_t values = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		for (size_t index = 0; index < values; ++index)
		{
			if (!std::isfinite(pixels.get()[index]) || pixels.get()[index] < 0.0f)
			{
				throw std::invalid_argument("HDR image contains invalid radiance");
			}
		}
		return {static_cast<uint32_t>(width), static_cast<uint32_t>(height), {pixels.get(), pixels.get() + values}};
	}
} // namespace Aster::AssetDetail

namespace Aster
{
	AssetImporter::AssetImporter(std::filesystem::path projectRoot)
		: m_ProjectRoot(std::filesystem::weakly_canonical(std::move(projectRoot)))
	{
		if (!std::filesystem::is_directory(m_ProjectRoot))
		{
			throw std::invalid_argument("Asset project root must be an existing directory");
		}
	}

	std::filesystem::path AssetImporter::ResolvePath(const std::filesystem::path& relativePath) const
	{
		return AssetDetail::ResolveContained(m_ProjectRoot, relativePath);
	}

	ImageAsset AssetImporter::LoadImage(const std::filesystem::path& relativePath) const
	{
		return AssetDetail::DecodeImage(AssetDetail::ReadBytes(ResolvePath(relativePath)));
	}

	HDRImageAsset AssetImporter::LoadHDR(const std::filesystem::path& relativePath) const
	{
		return AssetDetail::DecodeHDR(AssetDetail::ReadBytes(ResolvePath(relativePath)));
	}
} // namespace Aster
