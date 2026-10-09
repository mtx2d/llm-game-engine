#pragma once

#include "Aster/Assets/AssetImporter.h"

#include <span>

namespace Aster::AssetDetail
{
	inline constexpr size_t s_MaxAssetBytes = 256ULL * 1024ULL * 1024ULL;
	inline constexpr size_t s_MaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
	[[nodiscard]] std::filesystem::path ResolveContained(const std::filesystem::path& root,
														 const std::filesystem::path& relative);
	[[nodiscard]] std::vector<uint8_t> ReadBytes(const std::filesystem::path& path);
	[[nodiscard]] std::vector<uint8_t> ReadURI(const std::filesystem::path& root, const std::filesystem::path& source,
											   const std::string& uri,
											   std::vector<std::filesystem::path>& dependencies);
	[[nodiscard]] ImageAsset DecodeImage(std::span<const uint8_t> bytes);
} // namespace Aster::AssetDetail
