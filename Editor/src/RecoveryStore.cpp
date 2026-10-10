#include <Aster/Core/Hash.h>
#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/EditorStorage.h>
#include <Aster/Editor/RecoveryStore.h>
#include <Aster/Scene/Scene.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace Aster
{
	namespace
	{
		using Path = std::filesystem::path;
		using Json = nlohmann::json;
		constexpr uint64_t s_MaxSceneBytes = 64ULL * 1024 * 1024;
		constexpr uint64_t s_MaxManifestBytes = 64 * 1024;

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::runtime_error("Recovery: " + message);
			}
		}

		bool IsHex(std::string_view value, size_t length)
		{
			return value.size() == length && std::all_of(value.begin(), value.end(),
														 [](char character) {
															 return (character >= '0' && character <= '9') ||
																	(character >= 'a' && character <= 'f');
														 });
		}

		bool Exists(const Path& path)
		{
			return std::filesystem::exists(std::filesystem::symlink_status(path));
		}

		bool IsTemporary(std::string_view name)
		{
			for (const auto* prefix : {"0.aster.aster-tmp-", "1.aster.aster-tmp-", "Manifest.json.aster-tmp-"})
			{
				if (name.starts_with(prefix))
				{
					const auto suffix = name.substr(std::string_view(prefix).size());
					return !suffix.empty() && suffix.size() <= 32 &&
						   std::all_of(suffix.begin(), suffix.end(),
									   [](char value) { return (value >= '0' && value <= '9') || value == '-'; });
				}
			}
			return false;
		}

		void ValidateFile(const Path& path, uint64_t maximum)
		{
			const auto status = std::filesystem::symlink_status(path);
			Require(std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status) &&
						std::filesystem::canonical(path) == path,
					"Checkpoint file must be regular without links: " + path.string());
			Require(std::filesystem::hard_link_count(path) == 1, "Checkpoint file has aliases: " + path.string());
			Require(std::filesystem::file_size(path) <= maximum,
					"Checkpoint file exceeds its size limit: " + path.string());
		}

		std::vector<Path> SessionFiles(const Path& directory, bool enforceSizes = true)
		{
			Require(PrepareEditorDirectory(directory) == directory, "Session directory changed location");
			std::vector<Path> files;
			for (const auto& entry : std::filesystem::directory_iterator(directory))
			{
				Require(files.size() < 8, "Session contains excessive files: " + directory.filename().string());
				const auto name = entry.path().filename().string();
				Require(name == "Owner.lock" || name == "Manifest.json" || name == "0.aster" || name == "1.aster" ||
							IsTemporary(name),
						"Unknown session file: " + name);
				ValidateFile(entry.path(), enforceSizes
											   ? (name == "Manifest.json" ? s_MaxManifestBytes : s_MaxSceneBytes)
											   : std::numeric_limits<uint64_t>::max());
				files.push_back(entry.path());
			}
			return files;
		}

		std::vector<Path> Sessions(const Path& root, size_t maximum)
		{
			std::vector<Path> sessions;
			for (const auto& entry : std::filesystem::directory_iterator(root))
			{
				const auto name = entry.path().filename().string();
				if (name == "Catalog.lock")
				{
					continue;
				}
				Require(IsHex(name, 32), "Unknown catalog entry: " + name);
				Require(sessions.size() < maximum, "Session count exceeds the configured limit");
				Require(PrepareEditorDirectory(entry.path()) == entry.path(), "Invalid session directory");
				sessions.push_back(entry.path());
			}
			std::sort(sessions.begin(), sessions.end());
			return sessions;
		}

		void ValidateIdentity(const std::optional<std::string>& path, const std::optional<std::string>& source,
							  const std::string& savedDigest)
		{
			Require(path.has_value() == source.has_value(), "Document path and source digest must occur together");
			if (path)
			{
				Scene::ValidateAssetPath(*path);
				Require(path->find('\\') == std::string::npos && IsHex(*source, 64),
						"Invalid document source identity");
			}
			Require(IsHex(savedDigest, 64), "Invalid saved-scene digest");
		}

		Json ReadManifest(const Path& directory)
		{
			ValidateFile(directory / "Manifest.json", s_MaxManifestBytes);
			auto manifest = ReadJsonFile(directory / "Manifest.json", s_MaxManifestBytes, 8);
			const std::set<std::string> fields = {
				"Version", "Path", "SourceDigest", "SavedDigest", "Checkpoint", "CheckpointDigest", "Name"};
			Require(manifest.is_object() && manifest.size() == fields.size(), "Unsupported manifest fields");
			for (const auto& field : fields)
			{
				Require(manifest.contains(field), "Missing manifest field: " + field);
			}
			Require(manifest.at("Version").is_number_integer() && manifest.at("Version") == 1,
					"Unsupported manifest version");
			std::optional<std::string> path;
			std::optional<std::string> source;
			if (!manifest.at("Path").is_null())
			{
				path = manifest.at("Path").get<std::string>();
			}
			if (!manifest.at("SourceDigest").is_null())
			{
				source = manifest.at("SourceDigest").get<std::string>();
			}
			ValidateIdentity(path, source, manifest.at("SavedDigest").get<std::string>());
			const auto file = manifest.at("Checkpoint").get<std::string>();
			Require(file == "0.aster" || file == "1.aster", "Invalid checkpoint filename");
			const auto digest = manifest.at("CheckpointDigest").get<std::string>();
			Require(IsHex(digest, 64) && digest != manifest.at("SavedDigest").get<std::string>(),
					"Invalid checkpoint digest");
			(void)Scene(manifest.at("Name").get<std::string>());
			ValidateFile(directory / file, s_MaxSceneBytes);
			return manifest;
		}

		std::string NewSessionID()
		{
			std::random_device random;
			constexpr std::string_view digits = "0123456789abcdef";
			std::string id(32, '0');
			for (size_t part = 0; part < 4; ++part)
			{
				const auto value = static_cast<uint32_t>(random());
				for (size_t digit = 0; digit < 8; ++digit)
				{
					id[part * 8 + digit] = digits[(value >> (digit * 4)) & 15];
				}
			}
			return id;
		}

		bool CreateSessionDirectory(const Path& directory)
		{
#ifdef _WIN32
			return std::filesystem::create_directory(directory);
#else
			if (mkdir(directory.c_str(), 0700) == 0)
			{
				return true;
			}
			if (errno == EEXIST)
			{
				return false;
			}
			throw std::system_error(errno, std::generic_category(),
									"Cannot create recovery session: " + directory.string());
#endif
		}
	} // namespace

	RecoveryStore::RecoveryStore(Path assetRoot, RecoveryLimits limits)
		: m_AssetRoot(std::filesystem::canonical(std::move(assetRoot))), m_Root(m_AssetRoot / ".aster/Recovery"),
		  m_Limits(limits)
	{
		Require(std::filesystem::is_directory(m_AssetRoot), "Asset root must be an existing directory");
		Require(limits.MaximumSessions >= 1 && limits.MaximumSessions <= 64 && limits.MaximumBytes >= 1 &&
					limits.MaximumBytes <= 4ULL * 1024 * 1024 * 1024,
				"Invalid recovery storage limits");
	}

	bool RecoveryStore::PrepareRoot(bool create)
	{
		if (!create && !Exists(m_AssetRoot / ".aster"))
		{
			return false;
		}
		(void)PrepareEditorStorageDirectory(m_AssetRoot);
		if (!create && !Exists(m_Root))
		{
			return false;
		}
		Require(PrepareEditorDirectory(m_Root) == m_Root, "Recovery directory changed location");
		return true;
	}

	std::unique_ptr<FileLock> RecoveryStore::LockCatalog()
	{
		auto lock = FileLock::TryAcquire(m_Root / "Catalog.lock");
		Require(lock != nullptr, "Another editor is updating recovery storage; retry shortly");
		return lock;
	}

	Json RecoveryStore::List()
	{
		Json entries = Json::array();
		if (!PrepareRoot(false))
		{
			return entries;
		}
		const auto catalog = LockCatalog();
		for (const auto& directory : Sessions(m_Root, m_Limits.MaximumSessions))
		{
			const auto id = directory.filename().string();
			Json entry = {{"id", id}, {"active", false}, {"name", nullptr}, {"path", nullptr}, {"error", nullptr}};
			try
			{
				auto owner = FileLock::TryAcquire(directory / "Owner.lock");
				if (!owner)
				{
					entry["active"] = true;
				}
				else if (!Exists(directory / "Manifest.json"))
				{
					continue;
				}
				else
				{
					(void)SessionFiles(directory);
					const auto manifest = ReadManifest(directory);
					entry["name"] = manifest.at("Name");
					entry["path"] = manifest.at("Path");
				}
			}
			catch (const std::exception& error)
			{
				entry["error"] = error.what();
			}
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	RecoveryDocument RecoveryStore::Load(const std::string& sessionID)
	{
		Require(IsHex(sessionID, 32), "Invalid recovery session ID");
		Require(PrepareRoot(false), "No recovery storage exists");
		const auto catalog = LockCatalog();
		const auto directory = m_Root / sessionID;
		Require(Exists(directory), "Recovery session does not exist");
		(void)SessionFiles(directory);
		const auto owner = FileLock::TryAcquire(directory / "Owner.lock");
		Require(owner != nullptr, "Session belongs to a live editor");
		const auto manifest = ReadManifest(directory);
		const auto contents = ReadTextFile(directory / manifest.at("Checkpoint").get<std::string>(), s_MaxSceneBytes);
		Require(ComputeSha256(contents) == manifest.at("CheckpointDigest").get<std::string>(),
				"Checkpoint checksum mismatch");
		auto scene = Scene::Deserialize(ParseJson(contents));
		Require(scene.GetName() == manifest.at("Name").get<std::string>(),
				"Checkpoint scene name does not match its manifest");
		RecoveryDocument document;
		document.Scene = scene.Serialize();
		if (!manifest.at("Path").is_null())
		{
			document.Path = manifest.at("Path").get<std::string>();
			document.SourceDigest = manifest.at("SourceDigest").get<std::string>();
		}
		document.SavedDigest = manifest.at("SavedDigest").get<std::string>();
		return document;
	}

	void RecoveryStore::RemoveSession(const Path& directory, std::unique_ptr<FileLock>& owner)
	{
		const auto files = SessionFiles(directory, false);
		std::filesystem::remove(directory / "Manifest.json");
		for (const auto& file : files)
		{
			if (file.filename() != "Owner.lock" && file.filename() != "Manifest.json")
			{
				std::filesystem::remove(file);
			}
		}
		// Catalog ownership serializes every future acquisition of a session's
		// owner file, so an inactive session can now be removed without splitting
		// a live lock identity. Active sessions never pass the owner acquisition.
		owner.reset();
		std::filesystem::remove(directory / "Owner.lock");
		std::filesystem::remove(directory);
	}

	void RecoveryStore::Discard(const std::string& sessionID)
	{
		Require(IsHex(sessionID, 32), "Invalid recovery session ID");
		Require(PrepareRoot(false), "No recovery storage exists");
		const auto catalog = LockCatalog();
		const auto directory = m_Root / sessionID;
		Require(Exists(directory), "Recovery session does not exist");
		Require(PrepareEditorDirectory(directory) == directory, "Invalid session directory");
		auto owner = FileLock::TryAcquire(directory / "Owner.lock");
		Require(owner != nullptr, "Session belongs to a live editor");
		RemoveSession(directory, owner);
	}

	void RecoveryStore::Clear()
	{
		if (m_SessionID.empty())
		{
			return;
		}
		Require(PrepareRoot(false), "Owned recovery storage disappeared");
		const auto catalog = LockCatalog();
		const auto directory = m_Root / m_SessionID;
		if (Exists(directory))
		{
			if (!m_Owner)
			{
				m_Owner = FileLock::TryAcquire(directory / "Owner.lock");
				Require(m_Owner != nullptr, "Owned recovery session changed ownership");
			}
			RemoveSession(directory, m_Owner);
		}
		m_Owner.reset();
		m_SessionID.clear();
	}

	void RecoveryStore::Checkpoint(const RecoveryDocument& document)
	{
		ValidateIdentity(document.Path, document.SourceDigest, document.SavedDigest);
		const auto scene = Scene::Deserialize(document.Scene);
		const auto contents = document.Scene.dump();
		Require(contents.size() <= s_MaxSceneBytes, "Scene exceeds the 64 MiB checkpoint limit");
		const auto digest = ComputeSha256(contents);
		Require(digest != document.SavedDigest, "Clean documents do not need recovery checkpoints");
		(void)PrepareRoot(true);
		const auto catalog = LockCatalog();
		auto sessions = Sessions(m_Root, m_Limits.MaximumSessions);
		for (auto iterator = sessions.begin(); iterator != sessions.end();)
		{
			if (iterator->filename() != m_SessionID && !Exists(*iterator / "Manifest.json"))
			{
				auto owner = FileLock::TryAcquire(*iterator / "Owner.lock");
				if (owner)
				{
					RemoveSession(*iterator, owner);
					iterator = sessions.erase(iterator);
					continue;
				}
			}
			++iterator;
		}
		if (m_SessionID.empty())
		{
			Require(sessions.size() < m_Limits.MaximumSessions,
					"Session limit reached; explicitly discard old recovery work");
			for (int attempt = 0; attempt < 16 && m_SessionID.empty(); ++attempt)
			{
				auto id = NewSessionID();
				const auto directory = m_Root / id;
				if (CreateSessionDirectory(directory))
				{
					(void)PrepareEditorDirectory(directory);
					auto owner = FileLock::TryAcquire(directory / "Owner.lock");
					Require(owner != nullptr, "New session ownership was taken");
					m_SessionID = std::move(id);
					m_Owner = std::move(owner);
					sessions.push_back(directory);
				}
			}
			Require(!m_SessionID.empty(), "Cannot allocate an exclusive session directory");
		}
		const auto directory = m_Root / m_SessionID;
		Require(m_Owner != nullptr, "Recovery session is no longer owned");
		std::string previous;
		if (Exists(directory / "Manifest.json"))
		{
			previous = ReadManifest(directory).at("Checkpoint").get<std::string>();
		}
		for (const auto& file : SessionFiles(directory))
		{
			const auto name = file.filename().string();
			if (IsTemporary(name) || ((name == "0.aster" || name == "1.aster") && name != previous))
			{
				std::filesystem::remove(file);
			}
		}
		const std::string filename = previous == "0.aster" ? "1.aster" : "0.aster";
		const Json manifest = {{"Version", 1},
							   {"Path", document.Path ? Json(*document.Path) : Json(nullptr)},
							   {"SourceDigest", document.SourceDigest ? Json(*document.SourceDigest) : Json(nullptr)},
							   {"SavedDigest", document.SavedDigest},
							   {"Checkpoint", filename},
							   {"CheckpointDigest", digest},
							   {"Name", scene.GetName()}};
		const auto manifestContents = manifest.dump();
		Require(manifestContents.size() <= s_MaxManifestBytes, "Manifest exceeds its size limit");
		uint64_t total = contents.size() + manifestContents.size();
		for (const auto& session : sessions)
		{
			for (const auto& file : SessionFiles(session))
			{
				total += std::filesystem::file_size(file);
			}
		}
		Require(total <= m_Limits.MaximumBytes, "Storage budget reached; explicitly discard old recovery work");
		WriteTextFileAtomically(directory / filename, contents);
		// Publication is last. A crash before it leaves the previous slot/manifest
		// intact. A crash afterward exposes the complete flushed new checkpoint.
		WriteTextFileAtomically(directory / "Manifest.json", manifestContents);
	}
} // namespace Aster
