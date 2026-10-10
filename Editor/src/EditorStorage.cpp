#include <Aster/Core/PrivateDirectory.h>
#include <Aster/Editor/EditorStorage.h>

namespace Aster
{
	std::filesystem::path PrepareEditorStorageDirectory(const std::filesystem::path& directory)
	{
		return PreparePrivateDirectory(std::filesystem::canonical(directory) / ".aster");
	}

	std::filesystem::path PrepareEditorDirectory(const std::filesystem::path& directory)
	{
		return PreparePrivateDirectory(directory);
	}
} // namespace Aster
