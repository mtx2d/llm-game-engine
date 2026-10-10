#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace Aster
{
	// Bounds the actual bytes read, rejects duplicate keys and excessive nesting,
	// and requires one complete JSON document with no trailing content.
	[[nodiscard]] nlohmann::json ReadJsonFile(const std::filesystem::path& path, size_t maximumBytes,
											  size_t maximumDepth = 128);

	// Creates an exclusive sibling temporary file, flushes its contents to the
	// filesystem, and atomically replaces the destination. Failures before rename
	// preserve the destination. This does not promise power-loss durability of the
	// parent directory entry on every filesystem.
	void WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);
} // namespace Aster
