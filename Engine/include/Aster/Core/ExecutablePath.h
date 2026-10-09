#pragma once

#include <filesystem>

namespace Aster
{
	// Returns the absolute path of the running executable, independent of cwd.
	// Resolves symbolic links. Throws with platform error details on failure.
	[[nodiscard]] std::filesystem::path GetExecutablePath();
} // namespace Aster
