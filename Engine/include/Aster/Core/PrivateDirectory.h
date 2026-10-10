#pragma once

#include <filesystem>

namespace Aster
{
	// Create/validate one named directory under an existing canonical parent.
	// New POSIX directories use mode 0700; links, junctions and files are rejected.
	[[nodiscard]] std::filesystem::path PreparePrivateDirectory(const std::filesystem::path& directory);
} // namespace Aster
