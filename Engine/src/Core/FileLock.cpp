#include <Aster/Core/FileLock.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Aster
{
	struct FileLock::State
	{
#ifdef _WIN32
		HANDLE Handle = INVALID_HANDLE_VALUE;
#else
		int Descriptor = -1;
#endif
		~State()
		{
#ifdef _WIN32
			if (Handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(Handle);
			}
#else
			if (Descriptor >= 0)
			{
				close(Descriptor);
			}
#endif
		}
	};

	FileLock::FileLock() : m_State(std::make_unique<State>()) {}
	FileLock::~FileLock() = default;

	std::unique_ptr<FileLock> FileLock::TryAcquire(const std::filesystem::path& path)
	{
		if (!path.has_filename() || path.filename() == "." || path.filename() == ".." ||
			path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		{
			throw std::invalid_argument("Lock path must name a regular file without null characters");
		}
		const auto parent = std::filesystem::canonical(path.parent_path().empty() ? "." : path.parent_path());
		const auto resolved = parent / path.filename();
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(resolved)))
		{
			throw std::invalid_argument("Lock file cannot be a symbolic link: " + resolved.string());
		}
		auto lock = std::unique_ptr<FileLock>(new FileLock());
#ifdef _WIN32
		lock->m_State->Handle =
			CreateFileW(resolved.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
						OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (lock->m_State->Handle == INVALID_HANDLE_VALUE)
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot open lock file: " + resolved.string());
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!GetFileInformationByHandle(lock->m_State->Handle, &information))
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Cannot inspect lock file: " + resolved.string());
		}
		if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
			GetFileType(lock->m_State->Handle) != FILE_TYPE_DISK || information.nNumberOfLinks != 1)
		{
			throw std::invalid_argument("Lock file must be a regular file without links or reparse points: " +
										resolved.string());
		}
		OVERLAPPED overlapped{};
		if (!LockFileEx(lock->m_State->Handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
						&overlapped))
		{
			const auto error = GetLastError();
			if (error == ERROR_LOCK_VIOLATION)
			{
				return nullptr;
			}
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Cannot acquire file lock: " + resolved.string());
		}
#else
		lock->m_State->Descriptor =
			open(resolved.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
		if (lock->m_State->Descriptor < 0)
		{
			throw std::system_error(errno, std::generic_category(), "Cannot open lock file: " + resolved.string());
		}
		struct stat information
		{
		};
		if (fstat(lock->m_State->Descriptor, &information) != 0)
		{
			throw std::system_error(errno, std::generic_category(), "Cannot inspect lock file: " + resolved.string());
		}
		if (!S_ISREG(information.st_mode))
		{
			throw std::invalid_argument("Lock file must be regular: " + resolved.string());
		}
		while (flock(lock->m_State->Descriptor, LOCK_EX | LOCK_NB) != 0)
		{
			const auto error = errno;
			if (error == EINTR)
			{
				continue;
			}
			if (error == EWOULDBLOCK || error == EAGAIN)
			{
				return nullptr;
			}
			throw std::system_error(error, std::generic_category(), "Cannot acquire file lock: " + resolved.string());
		}
		struct stat current
		{
		};
		if (lstat(resolved.c_str(), &current) != 0 || current.st_dev != information.st_dev ||
			current.st_ino != information.st_ino || current.st_nlink != 1)
		{
			throw std::runtime_error("Lock file was replaced or has aliases: " + resolved.string());
		}
#endif
		return lock;
	}
} // namespace Aster
