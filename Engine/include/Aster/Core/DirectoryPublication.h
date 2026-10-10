#pragma once

#include <filesystem>

namespace Aster
{
	// Atomically publish a complete sibling directory. Every existing destination
	// is rejected, including an empty directory or a link. Unsupported filesystem
	// operations fail explicitly; there is no check-then-overwrite fallback.
	void PublishDirectoryExclusively(const std::filesystem::path& source, const std::filesystem::path& destination);
	// Remove only an empty real directory, never a file or symbolic link. Returns
	// false when absent; nonempty directories and other failures throw.
	[[nodiscard]] bool RemoveEmptyDirectoryForPublication(const std::filesystem::path& path);
} // namespace Aster
