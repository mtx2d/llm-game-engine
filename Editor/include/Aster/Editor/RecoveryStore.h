#pragma once

#include <Aster/Core/FileLock.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Aster
{
	struct RecoveryDocument
	{
		nlohmann::json Scene;
		std::optional<std::string> Path;
		std::optional<std::string> SourceDigest;
		std::string SavedDigest;
	};

	struct RecoveryLimits
	{
		size_t MaximumSessions = 32;
		uint64_t MaximumBytes = 512ULL * 1024 * 1024;
	};

	// One lazily created checkpoint session per owner. Destruction only unlocks;
	// Clear/Discard are explicit. No live session can be restored or discarded.
	class RecoveryStore final
	{
	  public:
		explicit RecoveryStore(std::filesystem::path assetRoot, RecoveryLimits limits = {});
		[[nodiscard]] nlohmann::json List();
		[[nodiscard]] RecoveryDocument Load(const std::string& sessionID);
		void Checkpoint(const RecoveryDocument& document);
		void Discard(const std::string& sessionID);
		void Clear();

	  private:
		[[nodiscard]] bool PrepareRoot(bool create);
		[[nodiscard]] std::unique_ptr<FileLock> LockCatalog();
		void RemoveSession(const std::filesystem::path& directory, std::unique_ptr<FileLock>& owner);

		std::filesystem::path m_AssetRoot;
		std::filesystem::path m_Root;
		RecoveryLimits m_Limits;
		std::string m_SessionID;
		std::unique_ptr<FileLock> m_Owner;
	};
} // namespace Aster
