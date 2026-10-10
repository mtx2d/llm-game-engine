#include <Aster/Editor/EditorStorage.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace Aster
{
	std::filesystem::path PrepareEditorStorageDirectory(const std::filesystem::path& directory)
	{
		return PrepareEditorDirectory(std::filesystem::canonical(directory) / ".aster");
	}

	std::filesystem::path PrepareEditorDirectory(const std::filesystem::path& directory)
	{
		if (!directory.has_filename() || directory.filename() == "." || directory.filename() == ".." ||
			directory.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		{
			throw std::invalid_argument("Editor directory must have a name without null characters");
		}
		const auto parent = std::filesystem::canonical(directory.parent_path());
		const auto storage = parent / directory.filename();
#ifdef _WIN32
		if (!CreateDirectoryW(storage.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot create editor storage: " + storage.string());
		}
		const auto attributes = GetFileAttributesW(storage.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot inspect editor storage: " + storage.string());
		}
		if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
#else
		if (mkdir(storage.c_str(), 0700) != 0 && errno != EEXIST)
		{
			throw std::system_error(errno, std::generic_category(),
									"Cannot create editor storage: " + storage.string());
		}
		const auto status = std::filesystem::symlink_status(storage);
		if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
#endif
		{
			throw std::invalid_argument("Editor storage must be a directory without links: " + storage.string());
		}
		if (std::filesystem::canonical(storage) != storage)
		{
			throw std::invalid_argument("Editor storage changed location: " + storage.string());
		}
		return storage;
	}
} // namespace Aster
