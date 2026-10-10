#pragma once

#include <filesystem>
#include <memory>

namespace Aster
{
	// Cooperative exclusive lock on a stable regular file. Never remove/replace
	// that file while another process could use it. Ownership ends on destruction
	// or process exit, including crashes; no stale PID/sentinel cleanup is needed.
	class FileLock final
	{
	  public:
		// Nonblocking: nullptr means contention only. Invalid paths and native I/O
		// failures throw. The existing parent must be writable for a new lock file.
		[[nodiscard]] static std::unique_ptr<FileLock> TryAcquire(const std::filesystem::path& path);
		~FileLock();
		FileLock(const FileLock&) = delete;
		FileLock& operator=(const FileLock&) = delete;

	  private:
		FileLock();
		struct State;
		std::unique_ptr<State> m_State;
	};
} // namespace Aster
