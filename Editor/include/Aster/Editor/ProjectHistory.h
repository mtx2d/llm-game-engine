#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Aster
{
	// Editor metadata only. Each mutation rereads under a cooperative native lock
	// and conditionally publishes, so separate editor instances merge their lists.
	class ProjectHistory
	{
	  public:
		explicit ProjectHistory(const std::filesystem::path& stateDirectory);
		[[nodiscard]] std::vector<std::filesystem::path> List() const;
		void Remember(const std::filesystem::path& project);
		void Forget(const std::filesystem::path& project);

	  private:
		struct Snapshot
		{
			std::optional<std::string> Contents;
			std::vector<std::filesystem::path> Paths;
		};
		Snapshot Read() const;
		void Update(const std::filesystem::path& project, bool remember);
		std::filesystem::path m_StateDirectory;
		std::filesystem::path m_Storage;
	};

	// A snapshot, never a recursive scan. Non-project files, private metadata and
	// links are omitted; truncation is explicit when the scan budget is exhausted.
	[[nodiscard]] nlohmann::json BrowseProjects(const std::filesystem::path& directory);
} // namespace Aster
