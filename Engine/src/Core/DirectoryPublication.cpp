#include <Aster/Core/DirectoryPublication.h>

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#endif

namespace Aster
{
	namespace
	{
		std::filesystem::path ResolveDirectoryEntry(const std::filesystem::path& path)
		{
			if (!path.has_filename() || path.filename() == "." || path.filename() == ".." ||
				path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
			{
				throw std::invalid_argument("Directory publication requires named entries without null characters");
			}
			return std::filesystem::canonical(path.has_parent_path() ? path.parent_path() : ".") / path.filename();
		}
	} // namespace

	bool RemoveEmptyDirectoryForPublication(const std::filesystem::path& path)
	{
		const auto directory = ResolveDirectoryEntry(path);
		const auto status = std::filesystem::symlink_status(directory);
		if (status.type() == std::filesystem::file_type::not_found)
		{
			return false;
		}
		if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
		{
			throw std::invalid_argument("Publication can remove only an empty real directory");
		}
#ifdef _WIN32
		const auto raw = CreateFileW(directory.c_str(), DELETE | FILE_READ_ATTRIBUTES,
									 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (raw == INVALID_HANDLE_VALUE)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			{
				return false;
			}
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Cannot open publication directory");
		}
		const std::unique_ptr<void, decltype(&CloseHandle)> handle(raw, CloseHandle);
		BY_HANDLE_FILE_INFORMATION information{};
		if (!GetFileInformationByHandle(handle.get(), &information))
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Inspect publication directory");
		}
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
			(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			throw std::invalid_argument("Publication cleanup requires a stable real directory");
		}
		FILE_DISPOSITION_INFO disposition{TRUE};
		if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot remove empty publication directory");
		}
		return true;
#else
		if (rmdir(directory.c_str()) == 0)
		{
			return true;
		}
		if (errno == ENOENT)
		{
			return false;
		}
		throw std::system_error(errno, std::generic_category(), "Cannot remove empty publication directory");
#endif
	}

	void PublishDirectoryExclusively(const std::filesystem::path& source, const std::filesystem::path& destination)
	{
		const auto from = ResolveDirectoryEntry(source);
		const auto to = ResolveDirectoryEntry(destination);
		if (from == to || from.parent_path() != to.parent_path() ||
			!std::filesystem::is_directory(std::filesystem::symlink_status(from)) ||
			std::filesystem::canonical(from) != from)
		{
			throw std::invalid_argument(
				"Directory publication requires a real source and a distinct sibling destination");
		}
		// Darwin's exclusive rename permits a case-only rename of the source on
		// case-insensitive volumes. Reject that existing alias as well. The native
		// no-replace operation below still protects destinations appearing later.
		if (std::filesystem::exists(std::filesystem::symlink_status(to)))
		{
			throw std::runtime_error("Directory publication destination already exists: " + to.string());
		}
#ifdef _WIN32
		if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot exclusively publish directory: " + to.string());
		}
#else
#ifdef __APPLE__
		const int result = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__)
		const int result = renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
#else
#error "Aster directory publication requires a supported native exclusive rename backend"
#endif
		if (result != 0)
		{
			throw std::system_error(errno, std::generic_category(),
									"Cannot exclusively publish directory: " + to.string());
		}
#endif
	}
} // namespace Aster
