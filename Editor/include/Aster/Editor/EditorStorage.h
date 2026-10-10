#pragma once

#include <filesystem>

namespace Aster
{
	// Create/validate private editor storage under an existing canonical directory.
	// Symlinks, junctions and non-directories are rejected before storing anything.
	[[nodiscard]] std::filesystem::path PrepareEditorStorageDirectory(const std::filesystem::path& directory);
} // namespace Aster
