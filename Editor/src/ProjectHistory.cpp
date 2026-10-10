#include <Aster/Core/FileLock.h>
#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/EditorStorage.h>
#include <Aster/Editor/ProjectHistory.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace Aster
{
	namespace
	{
		constexpr std::size_t s_MaximumProjects = 16;
		constexpr std::size_t s_MaximumHistoryBytes = 128 * 1024;

		void ValidateProjectPath(const std::filesystem::path& path)
		{
			const auto text = path.generic_string();
			if (!path.is_absolute() || path.extension() != ".asterproj" || text.size() > 4095 ||
				text.find('\0') != std::string::npos || path.lexically_normal() != path)
			{
				throw std::invalid_argument("Recent project must be a normalized absolute .asterproj path");
			}
			(void)nlohmann::json(text).dump(); // Validate UTF-8 before persistence.
		}
	} // namespace

	ProjectHistory::ProjectHistory(const std::filesystem::path& stateDirectory)
		: m_StateDirectory(std::filesystem::canonical(stateDirectory)),
		  m_Storage(PrepareEditorStorageDirectory(m_StateDirectory))
	{
		(void)Read();
	}

	ProjectHistory::Snapshot ProjectHistory::Read() const
	{
		if (PrepareEditorStorageDirectory(m_StateDirectory) != m_Storage)
		{
			throw std::runtime_error("Editor history storage changed location");
		}
		const auto file = m_Storage / "Projects.json";
		if (std::filesystem::is_symlink(file))
		{
			throw std::invalid_argument("Editor history must not be a symbolic link");
		}
		Snapshot result;
		if (!std::filesystem::exists(file))
		{
			return result;
		}
		if (std::filesystem::canonical(file) != file)
		{
			throw std::invalid_argument("Editor history changed physical location");
		}
		result.Contents = ReadTextFile(file, s_MaximumHistoryBytes);
		const auto document = ParseJson(*result.Contents, 4);
		if (!document.is_object() || document.size() != 2 || !document.contains("Version") ||
			!document.at("Version").is_number_integer() || document.at("Version") != 1 ||
			!document.contains("Projects") || !document.at("Projects").is_array() ||
			document.at("Projects").size() > s_MaximumProjects)
		{
			throw std::invalid_argument("Unsupported recent-project history format or count");
		}
		for (const auto& entry : document.at("Projects"))
		{
			const std::filesystem::path path(entry.get<std::string>());
			ValidateProjectPath(path);
			if (std::find(result.Paths.begin(), result.Paths.end(), path) != result.Paths.end())
			{
				throw std::invalid_argument("Duplicate recent project path");
			}
			result.Paths.push_back(path);
		}
		return result;
	}

	std::vector<std::filesystem::path> ProjectHistory::List() const
	{
		return Read().Paths;
	}

	void ProjectHistory::Remember(const std::filesystem::path& project)
	{
		Update(project, true);
	}

	void ProjectHistory::Forget(const std::filesystem::path& project)
	{
		Update(project, false);
	}

	void ProjectHistory::Update(const std::filesystem::path& project, bool remember)
	{
		ValidateProjectPath(project);
		if (PrepareEditorStorageDirectory(m_StateDirectory) != m_Storage)
		{
			throw std::runtime_error("Editor history storage changed location");
		}
		const auto lock = FileLock::TryAcquire(m_Storage / "Projects.lock");
		if (!lock)
		{
			throw std::runtime_error("Another editor is updating recent projects; retry");
		}
		auto snapshot = Read();
		std::erase(snapshot.Paths, project);
		if (remember)
		{
			snapshot.Paths.insert(snapshot.Paths.begin(), project);
			snapshot.Paths.resize(std::min(snapshot.Paths.size(), s_MaximumProjects));
		}
		auto paths = nlohmann::json::array();
		for (const auto& path : snapshot.Paths)
		{
			paths.push_back(path.generic_string());
		}
		const auto contents = nlohmann::json({{"Version", 1}, {"Projects", paths}}).dump(2) + "\n";
		if (contents.size() > s_MaximumHistoryBytes)
		{
			throw std::invalid_argument("Recent-project history exceeds its byte limit");
		}
		WriteTextFileConditionally(m_Storage / "Projects.json", contents,
								   snapshot.Contents ? std::optional<std::string_view>(*snapshot.Contents)
													 : std::nullopt,
								   s_MaximumHistoryBytes);
	}

	nlohmann::json BrowseProjects(const std::filesystem::path& directory)
	{
		if (directory.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		{
			throw std::invalid_argument("Project browser path contains a null character");
		}
		const auto root = std::filesystem::canonical(directory);
		if (root.generic_string().size() > 4095)
		{
			throw std::invalid_argument("Project browser directory exceeds its 4095-byte input limit");
		}
		(void)nlohmann::json(root.generic_string()).dump();
		std::vector<nlohmann::json> entries;
		bool truncated = false;
		std::size_t inspected = 0;
		for (const auto& entry : std::filesystem::directory_iterator(root))
		{
			if (++inspected > 4096)
			{
				truncated = true;
				break;
			}
			auto name = entry.path().filename().generic_string();
			auto folded = name;
			std::transform(folded.begin(), folded.end(), folded.begin(),
						   [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch + 'a' - 'A') : char(ch); });
			const auto status = entry.symlink_status();
			if (folded == ".aster" || std::filesystem::is_symlink(status))
			{
				continue;
			}
			const bool isDirectory = std::filesystem::is_directory(status);
			if (!isDirectory && (!std::filesystem::is_regular_file(status) || entry.path().extension() != ".asterproj"))
			{
				continue;
			}
			(void)nlohmann::json(name).dump();
			if (entry.path().generic_string().size() > 4095)
			{
				throw std::invalid_argument("Project browser entry exceeds its 4095-byte path limit");
			}
			entries.push_back({{"name", name}, {"path", entry.path().generic_string()}, {"directory", isDirectory}});
		}
		std::sort(entries.begin(), entries.end(),
				  [](const auto& first, const auto& second)
				  {
					  if (first.at("directory") != second.at("directory"))
					  {
						  return first.at("directory").template get<bool>();
					  }
					  return first.at("name").template get<std::string>() <
							 second.at("name").template get<std::string>();
				  });
		return {{"directory", root.generic_string()},
				{"parent", root == root.root_path() ? nlohmann::json(nullptr)
													: nlohmann::json(root.parent_path().generic_string())},
				{"entries", entries},
				{"truncated", truncated}};
	}
} // namespace Aster
