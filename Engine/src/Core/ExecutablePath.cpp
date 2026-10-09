#include <Aster/Core/ExecutablePath.h>

#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Aster
{
	std::filesystem::path GetExecutablePath()
	{
#if defined(_WIN32)
		std::vector<wchar_t> buffer(1024);
		while (buffer.size() <= 1024 * 1024)
		{
			const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
										"Cannot locate executable");
			}
			if (length < buffer.size())
			{
				return std::filesystem::canonical(std::filesystem::path(buffer.data(), buffer.data() + length));
			}
			buffer.resize(buffer.size() * 2);
		}
		throw std::runtime_error("Executable path exceeds the supported size");
#elif defined(__APPLE__)
		std::uint32_t size = 0;
		(void)_NSGetExecutablePath(nullptr, &size);
		if (size == 0 || size > 1024 * 1024)
		{
			throw std::runtime_error("Cannot determine executable path size");
		}
		std::vector<char> buffer(size);
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		{
			throw std::runtime_error("Cannot locate executable");
		}
		return std::filesystem::canonical(buffer.data());
#elif defined(__linux__)
		return std::filesystem::canonical("/proc/self/exe");
#else
#error Aster executable location requires an implementation for this platform.
#endif
	}
} // namespace Aster
