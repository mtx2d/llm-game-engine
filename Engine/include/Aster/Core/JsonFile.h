#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Aster
{
	[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, size_t maximumBytes);
	[[nodiscard]] nlohmann::json ParseJson(std::string_view text, size_t maximumDepth = 128);
	// Bounds the actual bytes read, rejects duplicate keys and excessive nesting,
	// and requires one complete JSON document with no trailing content.
	[[nodiscard]] nlohmann::json ReadJsonFile(const std::filesystem::path& path, size_t maximumBytes,
											  size_t maximumDepth = 128);

	// Creates an exclusive sibling temporary file, flushes its contents to the
	// filesystem, and atomically replaces the destination. Failures before rename
	// preserve the destination. This does not promise power-loss durability of the
	// parent directory entry on every filesystem.
	void WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);

	// Absent expected contents require exclusive publication of a new file.
	// Otherwise compare exact bytes immediately before atomic replacement.
	// This detects observed changes; it is not atomic compare-and-swap against
	// arbitrary concurrent writers.
	void WriteTextFileConditionally(const std::filesystem::path& path, std::string_view text,
									std::optional<std::string_view> expectedContents,
									size_t maximumBytes = 64ULL * 1024ULL * 1024ULL);
} // namespace Aster
