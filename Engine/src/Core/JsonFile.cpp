#include <Aster/Core/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Aster
{
	std::string ReadTextFile(const std::filesystem::path& path, size_t maximumBytes)
	{
		if (maximumBytes == 0 || maximumBytes > 64ULL * 1024ULL * 1024ULL)
		{
			throw std::invalid_argument("Invalid text file size limit");
		}
		if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > maximumBytes)
		{
			throw std::invalid_argument("Input is not a regular file within its size limit: " + path.string());
		}
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			throw std::runtime_error("Cannot open text file: " + path.string());
		}
		std::string text;
		std::array<char, 8192> buffer{};
		while (input)
		{
			const auto count = std::min(buffer.size(), maximumBytes + 1 - text.size());
			input.read(buffer.data(), static_cast<std::streamsize>(count));
			text.append(buffer.data(), static_cast<size_t>(input.gcount()));
			if (text.size() > maximumBytes)
			{
				throw std::invalid_argument("File grew beyond its size limit: " + path.string());
			}
		}
		if (input.bad() || !input.eof())
		{
			throw std::runtime_error("Cannot read text file: " + path.string());
		}
		return text;
	}

	nlohmann::json ParseJson(std::string_view text, size_t maximumDepth)
	{
		if (text.size() > 64ULL * 1024ULL * 1024ULL || maximumDepth == 0 || maximumDepth > 128)
		{
			throw std::invalid_argument("Invalid JSON input size or depth limit");
		}
		std::vector<std::set<std::string>> objectKeys;
		const auto validate = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
		{
			if (depth < 0 || static_cast<size_t>(depth) > maximumDepth)
			{
				throw std::invalid_argument("JSON nesting exceeds its depth limit");
			}
			if (event == nlohmann::json::parse_event_t::object_start)
			{
				objectKeys.emplace_back();
			}
			else if (event == nlohmann::json::parse_event_t::object_end)
			{
				objectKeys.pop_back();
			}
			else if (event == nlohmann::json::parse_event_t::key &&
					 !objectKeys.back().insert(value.get<std::string>()).second)
			{
				throw std::invalid_argument("Duplicate JSON key: " + value.get<std::string>());
			}
			return true;
		};
		return nlohmann::json::parse(text, validate);
	}

	nlohmann::json ReadJsonFile(const std::filesystem::path& path, size_t maximumBytes, size_t maximumDepth)
	{
		return ParseJson(ReadTextFile(path, maximumBytes), maximumDepth);
	}

	static void WriteTextFile(const std::filesystem::path& path, std::string_view text, bool conditional,
							  std::optional<std::string_view> expectedContents)
	{
		if (path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
			std::filesystem::is_symlink(path))
		{
			throw std::invalid_argument("Atomic output must name a file, not a symbolic link");
		}
		const auto parent = std::filesystem::canonical(path.has_parent_path() ? path.parent_path() : ".");
		const auto destination = parent / path.filename();
		std::random_device random;
		std::filesystem::path temporary;
#ifdef _WIN32
		HANDLE file = INVALID_HANDLE_VALUE;
#else
		int file = -1;
#endif
		for (int attempt = 0; attempt < 16; ++attempt)
		{
			temporary = destination;
			temporary += ".aster-tmp-" + std::to_string(random()) + "-" + std::to_string(random());
#ifdef _WIN32
			file =
				CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				break;
			}
			if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create output file");
			}
#else
			file = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
			if (file >= 0)
			{
				break;
			}
			if (errno != EEXIST)
			{
				throw std::system_error(errno, std::generic_category(), "Create output file");
			}
#endif
		}
#ifdef _WIN32
		if (file == INVALID_HANDLE_VALUE)
#else
		if (file < 0)
#endif
		{
			throw std::runtime_error("Cannot allocate an exclusive temporary output file");
		}
		try
		{
			size_t offset = 0;
			while (offset < text.size())
			{
				const auto count = std::min(text.size() - offset, size_t{1024 * 1024});
#ifdef _WIN32
				DWORD written = 0;
				if (!WriteFile(file, text.data() + offset, static_cast<DWORD>(count), &written, nullptr))
				{
					throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
											"Write output file");
				}
				if (written == 0)
				{
					throw std::runtime_error("Output file write made no progress");
				}
#else
				const auto written = write(file, text.data() + offset, count);
				if (written < 0 && errno == EINTR)
				{
					continue;
				}
				if (written <= 0)
				{
					throw std::system_error(written == 0 ? EIO : errno, std::generic_category(), "Write output file");
				}
#endif
				offset += static_cast<size_t>(written);
			}
#ifdef _WIN32
			if (!FlushFileBuffers(file))
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Flush output file");
			}
			const bool closed = CloseHandle(file) != 0;
			file = INVALID_HANDLE_VALUE;
			if (!closed)
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Close output file");
			}
#else
			int flushed = 0;
			do
			{
				flushed = fsync(file);
			} while (flushed < 0 && errno == EINTR);
			if (flushed < 0)
			{
				throw std::system_error(errno, std::generic_category(), "Flush output file");
			}
			const auto closed = close(file);
			file = -1;
			if (closed < 0)
			{
				throw std::system_error(errno, std::generic_category(), "Close output file");
			}
#endif
			if (conditional && expectedContents &&
				ReadTextFile(destination, 64ULL * 1024ULL * 1024ULL) != *expectedContents)
			{
				throw std::runtime_error(
					"Save conflict: file changed outside this editor; reload or save to a new path");
			}
#ifdef _WIN32
			const DWORD flags =
				MOVEFILE_WRITE_THROUGH | ((!conditional || expectedContents) ? MOVEFILE_REPLACE_EXISTING : 0);
			if (!MoveFileExW(temporary.c_str(), destination.c_str(), flags))
			{
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
										"Publish output file");
			}
#else
			if (conditional && !expectedContents)
			{
				// Same-directory linking publishes a complete file exclusively on
				// POSIX, where rename would replace a competing creator's file.
				std::filesystem::create_hard_link(temporary, destination);
				std::filesystem::remove(temporary);
			}
			else
			{
				std::filesystem::rename(temporary, destination);
			}
#endif
		}
		catch (...)
		{
#ifdef _WIN32
			if (file != INVALID_HANDLE_VALUE)
			{
				CloseHandle(file);
			}
#else
			if (file >= 0)
			{
				close(file);
			}
#endif
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			throw;
		}
	}

	void WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text)
	{
		WriteTextFile(path, text, false, std::nullopt);
	}

	void WriteTextFileConditionally(const std::filesystem::path& path, std::string_view text,
									std::optional<std::string_view> expectedContents)
	{
		WriteTextFile(path, text, true, expectedContents);
	}
} // namespace Aster
