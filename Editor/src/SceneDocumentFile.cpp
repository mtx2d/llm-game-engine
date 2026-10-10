#include <Aster/Core/FileLock.h>
#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/EditorStorage.h>
#include <Aster/Editor/SceneDocumentFile.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace Aster
{
	Scene SceneDocumentFile::Load(const std::filesystem::path& path)
	{
		auto resolved = std::filesystem::canonical(path);
		auto contents = ReadTextFile(resolved, 64ULL * 1024ULL * 1024ULL);
		auto scene = Scene::Deserialize(ParseJson(contents));
		m_Path = std::move(resolved);
		m_Contents = std::move(contents);
		return scene;
	}

	void SceneDocumentFile::Save(const Scene& scene, const std::filesystem::path& path)
	{
		auto resolved = std::filesystem::weakly_canonical(path);
		auto contents = scene.Serialize().dump(2);
		if (contents.size() > 64ULL * 1024ULL * 1024ULL)
		{
			throw std::invalid_argument("Scene exceeds the 64 MiB document size limit");
		}
		const bool sameFile = m_Path && *m_Path == resolved;
		const auto storage = PrepareEditorStorageDirectory(resolved.parent_path());
		const auto lock = FileLock::TryAcquire(storage / "Writes.lock");
		if (!lock)
		{
			throw std::runtime_error("Scene directory is being saved by another editor; retry saving shortly");
		}
		if (!sameFile && std::filesystem::exists(resolved))
		{
			throw std::runtime_error("Save As destination already exists; choose a new path or load it before editing");
		}
		WriteTextFileConditionally(resolved, contents,
								   sameFile ? std::optional<std::string_view>(m_Contents) : std::nullopt);
		m_Path = std::move(resolved);
		m_Contents = std::move(contents);
	}

	void SceneDocumentFile::VerifyUnchanged() const
	{
		if (m_Path && ReadTextFile(*m_Path, 64ULL * 1024ULL * 1024ULL) != m_Contents)
		{
			throw std::runtime_error("Save conflict: file changed outside this editor; reload or save to a new path");
		}
	}
} // namespace Aster
