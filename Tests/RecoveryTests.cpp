#include <Aster/Core/Hash.h>
#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/CommandProcessor.h>
#include <Aster/Editor/RecoveryStore.h>
#include <Aster/Scene/Scene.h>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
	using Json = nlohmann::json;
	using Path = std::filesystem::path;

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const char* message)
	{
		bool rejected = false;
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Check(rejected, message);
	}

	Json Execute(Aster::CommandProcessor& editor, const Json& request)
	{
		const auto response = editor.Execute(request);
		if (!response.at("ok").get<bool>())
		{
			throw std::runtime_error(response.at("error").get<std::string>());
		}
		return response.at("result");
	}

	Aster::RecoveryDocument Document(const std::string& name)
	{
		Aster::Scene scene(name);
		(void)scene.CreateEntity("Recovered entity");
		return {scene.Serialize(), std::nullopt, std::nullopt, Aster::ComputeSha256("null")};
	}

	void TestStore(const Path& root)
	{
		std::filesystem::create_directory(root);
		auto owner = std::make_unique<Aster::RecoveryStore>(root);
		Aster::RecoveryStore observer(root);
		Check(owner->List().empty() && !std::filesystem::exists(root / ".aster"),
			  "Read-only discovery creates no storage");
#ifndef _WIN32
		// A pre-existing parent must not broaden a newly created session's mode.
		std::filesystem::create_directories(root / ".aster/Recovery");
		std::filesystem::permissions(root / ".aster/Recovery", std::filesystem::perms::all);
#endif
		auto document = Document("First checkpoint");
		owner->Checkpoint(document);
		const auto active = observer.List();
		Check(active.size() == 1 && active[0].at("active"), "Live owner is visible as active");
		const auto id = active[0].at("id").get<std::string>();
#ifndef _WIN32
		const auto permissions = std::filesystem::status(root / ".aster/Recovery" / id).permissions();
		Check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
				  std::filesystem::perms::none,
			  "New session has owner-only permissions even under a broadly accessible parent");
#endif
		Rejects([&] { (void)observer.Load(id); }, "A live checkpoint cannot be recovered");
		Rejects([&] { observer.Discard(id); }, "A live checkpoint cannot be discarded");
		document.Scene["Name"] = "Second checkpoint";
		owner->Checkpoint(document);
		const auto directory = root / ".aster/Recovery" / id;
		const auto manifestPath = directory / "Manifest.json";
		const auto published = Aster::ReadTextFile(manifestPath, 64 * 1024);
		auto invalid = document;
		invalid.SavedDigest = Aster::ComputeSha256(invalid.Scene.dump());
		Rejects([&] { owner->Checkpoint(invalid); }, "Clean checkpoint rejected");
		invalid = document;
		invalid.Scene["Version"] = 999;
		Rejects([&] { owner->Checkpoint(invalid); }, "Unsupported scene checkpoint rejected");
		Check(Aster::ReadTextFile(manifestPath, 64 * 1024) == published, "Rejected checkpoints preserve publication");
		owner.reset();
		Check(observer.List().size() == 1 && !observer.List()[0].at("active").get<bool>(),
			  "Released owner becomes recoverable");
		Check(observer.Load(id).Scene == document.Scene, "Latest complete checkpoint is recovered");
		const auto manifest = Json::parse(published);
		const auto checkpoint = directory / manifest.at("Checkpoint").get<std::string>();
		const auto sceneBytes = Aster::ReadTextFile(checkpoint, 64ULL * 1024 * 1024);
		const auto unpublished = manifest.at("Checkpoint") == "0.aster" ? "1.aster" : "0.aster";
		Aster::WriteTextFileAtomically(directory / unpublished, Document("Unpublished").Scene.dump());
		Aster::WriteTextFileAtomically(directory / "Manifest.json.aster-tmp-12-34", "partial");
		Check(observer.Load(id).Scene == document.Scene, "Interrupted publication retains previous manifest/slot");
		for (const auto& change :
			 std::vector<Json>{{{"Version", 99}},
							   {{"Version", 1.0}},
							   {{"Version", true}},
							   {{"Checkpoint", "../Outside.aster"}},
							   {{"Path", "../Outside.aster"}, {"SourceDigest", std::string(64, 'a')}},
							   {{"SourceDigest", std::string(64, 'a')}},
							   {{"SavedDigest", "invalid"}},
							   {{"CheckpointDigest", std::string(64, 'a')}},
							   {{"Name", "Mismatched"}},
							   {{"Unknown", 1}}})
		{
			auto corrupt = manifest;
			corrupt.update(change);
			Aster::WriteTextFileAtomically(manifestPath, corrupt.dump());
			Rejects([&] { (void)observer.Load(id); }, "Malformed recovery metadata rejected");
		}
		Aster::WriteTextFileAtomically(manifestPath, published);
		Aster::WriteTextFileAtomically(checkpoint, Document("Tampered but valid JSON").Scene.dump());
		Rejects([&] { (void)observer.Load(id); }, "Valid JSON with wrong checksum rejected");
		Aster::WriteTextFileAtomically(checkpoint, sceneBytes);
		Aster::WriteTextFileAtomically(manifestPath, "{\"Version\":1,\"Version\":1}");
		Check(!observer.List()[0].at("error").is_null(), "Unreadable checkpoint is discoverable with a diagnostic");
		observer.Discard(id);
		Check(observer.List().empty(), "Explicit discard removes malformed owned data");
		Rejects([&] { observer.Discard("../outside"); }, "Escaping session IDs rejected");
		Rejects([&] { (void)observer.Load(std::string(32, 'G')); }, "Nonhex IDs rejected");

		Aster::RecoveryStore first(root, {1, 4096});
		Aster::RecoveryStore second(root, {1, 4096});
		first.Checkpoint(document);
		const auto limitedID = first.List()[0].at("id").get<std::string>();
		Rejects([&] { second.Checkpoint(document); }, "Session limit protects existing recovery work");
		document.Scene["Name"] = std::string(4096, 'x');
		const auto beforeBudget =
			Aster::ReadTextFile(root / ".aster/Recovery" / limitedID / "Manifest.json", 64 * 1024);
		Rejects([&] { first.Checkpoint(document); }, "Storage budget rejects oversized publication");
		Check(Aster::ReadTextFile(root / ".aster/Recovery" / limitedID / "Manifest.json", 64 * 1024) == beforeBudget,
			  "Budget rejection preserves prior checkpoint");
		first.Clear();
		document = Document("Budget retry");
		second.Checkpoint(document);
		second.Clear();
		Check(second.List().empty(), "Clear releases and removes own session");
	}

	void TestCommands(const Path& root)
	{
		const auto project = Aster::Project::Create(root / "Game", "Recovery");
		const auto scenePath = project.ResolveAssetPath("Scenes/Main.aster");
		const auto original = Aster::ReadTextFile(scenePath, 1024 * 1024);
		Json authored;
		std::string id;
		{
			Aster::CommandProcessor author(project.GetFilePath());
			author.EnableRecovery();
			Execute(author, {{"command", "history.begin"}});
			Execute(author, {{"command", "entity.create"}, {"name", "Unfinished"}});
			Check(!author.Execute({{"command", "recovery.checkpoint"}}).at("ok").get<bool>(),
				  "Unfinished edits cannot checkpoint");
			Check(!author.UpdateRecovery(true), "Automatic recovery defers unfinished groups");
			Execute(author, {{"command", "history.commit"}});
			Check(!author.UpdateRecovery(true), "Completed edit automatically checkpoints");
			authored = author.GetScene().Serialize();
			id = Execute(author, {{"command", "recovery.list"}})[0].at("id").get<std::string>();
			Check(Aster::ReadTextFile(scenePath, 1024 * 1024) == original, "Checkpoint never saves the scene file");
			const auto handle = author.GetScene().Entities().front();
			Check(!author.Execute({{"command", "recovery.restore"}, {"session", id}, {"discardChanges", true}})
						  .at("ok")
						  .get<bool>() &&
					  author.GetScene().IsAlive(handle) && author.GetScene().Serialize() == authored,
				  "Live restore rejection preserves handles and authored work");
		}
		{
			Aster::CommandProcessor editor(project.GetFilePath());
			const auto current = Execute(editor, {{"command", "entity.create"}, {"name", "Current unsaved work"}});
			const auto handle = editor.GetScene().FindByID(current.at("entity").get<uint64_t>());
			const auto beforeRestore = editor.GetScene().Serialize();
			Check(!editor.Execute({{"command", "recovery.restore"}, {"session", id}}).at("ok").get<bool>() &&
					  editor.GetScene().Serialize() == beforeRestore && editor.GetScene().IsAlive(handle),
				  "Recovery requires explicit discard of current unsaved work");
			const auto manifestPath = project.GetAssetDirectory() / ".aster/Recovery" / id / "Manifest.json";
			const auto manifestBytes = Aster::ReadTextFile(manifestPath, 64 * 1024);
			Aster::WriteTextFileAtomically(manifestPath, "invalid");
			Check(!editor.Execute({{"command", "recovery.restore"}, {"session", id}, {"discardChanges", true}})
						  .at("ok")
						  .get<bool>() &&
					  editor.GetScene().Serialize() == beforeRestore && editor.GetScene().IsAlive(handle),
				  "Failed restore preserves current authored work and handles");
			Execute(editor, {{"command", "history.undo"}});
			Check(!editor.GetScene().IsAlive(handle), "Failed restore preserves current undo history");
			Aster::WriteTextFileAtomically(manifestPath, manifestBytes);
			const auto result = Execute(editor, {{"command", "recovery.restore"}, {"session", id}});
			Check(result.at("warning").is_null() && editor.GetScenePath() == "Scenes/Main.aster" &&
					  editor.HasUnsavedChanges() && editor.GetScene().Serialize() == authored,
				  "Unchanged source restores original identity and dirty work");
			Check(!editor.Execute({{"command", "history.undo"}}).at("ok").get<bool>(), "Recovery isolates history");
			Execute(editor, {{"command", "scene.save"}, {"path", "Scenes/Main.aster"}});
			Check(!editor.UpdateRecovery(true), "Saving clears owned recovery checkpoint");
			Check(Aster::Scene::Load(scenePath).Serialize() == authored,
				  "Recovered work can save through original file baseline");
			Execute(editor, {{"command", "recovery.discard"}, {"session", id}});
			Check(Execute(editor, {{"command", "recovery.list"}}).empty(),
				  "Source checkpoint stays until explicit discard");
		}
		{
			Aster::CommandProcessor author(project.GetFilePath());
			Execute(author, {{"command", "entity.create"}, {"name", "Pending after external edit"}});
			Execute(author, {{"command", "recovery.checkpoint"}});
			id = Execute(author, {{"command", "recovery.list"}})[0].at("id").get<std::string>();
			authored = author.GetScene().Serialize();
		}
		auto external = Aster::Scene::Load(scenePath);
		external.SetName("External owner");
		external.Save(scenePath);
		const auto externalBytes = Aster::ReadTextFile(scenePath, 1024 * 1024);
		{
			Aster::CommandProcessor editor(project.GetFilePath());
			const auto result = Execute(editor, {{"command", "recovery.restore"}, {"session", id}});
			Check(!result.at("warning").is_null() && !editor.GetScenePath() && editor.HasUnsavedChanges() &&
					  editor.GetScene().Serialize() == authored,
				  "Changed source recovers an untitled dirty copy");
			Check(!editor.Execute({{"command", "scene.save"}, {"path", "Scenes/Main.aster"}}).at("ok").get<bool>(),
				  "Recovered copy cannot overwrite externally changed original");
			Check(Aster::ReadTextFile(scenePath, 1024 * 1024) == externalBytes,
				  "Recovery preserves external file bytes");
			Execute(editor, {{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}});
			Check(!editor.UpdateRecovery(true), "Recovered Save As clears current checkpoint");
			Execute(editor, {{"command", "recovery.discard"}, {"session", id}});
		}
	}

	void TestStorageFailures(const Path& root)
	{
		std::filesystem::create_directory(root);
		Aster::RecoveryStore store(root);
		auto document = Document("Protected checkpoint");
		store.Checkpoint(document);
		const auto id = store.List()[0].at("id").get<std::string>();
		const auto directory = root / ".aster/Recovery" / id;
		const auto manifestPath = directory / "Manifest.json";
		const auto published = Aster::ReadTextFile(manifestPath, 64 * 1024);
		{
			const auto catalog = Aster::FileLock::TryAcquire(root / ".aster/Recovery/Catalog.lock");
			Check(catalog != nullptr, "Catalog probe owns stable lock");
			Rejects([&] { store.Checkpoint(Document("Busy")); }, "Busy catalog rejects publication");
			Rejects([&] { store.Clear(); }, "Busy catalog rejects checkpoint cleanup");
			Check(Aster::ReadTextFile(manifestPath, 64 * 1024) == published,
				  "Busy recovery operations preserve published bytes");
		}
		Aster::WriteTextFileAtomically(directory / "Foreign.txt", "User-owned data");
		Rejects([&] { store.Clear(); }, "Unknown data prevents destructive cleanup");
		Check(Aster::ReadTextFile(directory / "Foreign.txt", 1024) == "User-owned data" &&
				  Aster::ReadTextFile(manifestPath, 64 * 1024) == published,
			  "Cleanup preflight preserves foreign files and manifest");
		std::filesystem::remove(directory / "Foreign.txt");
		const auto alias = root / "ManifestAlias.json";
		std::filesystem::create_hard_link(manifestPath, alias);
		Rejects([&] { store.Clear(); }, "Multiply linked checkpoint rejected before cleanup");
		Check(std::filesystem::exists(manifestPath) && Aster::ReadTextFile(alias, 64 * 1024) == published,
			  "Linked data remains intact");
		std::filesystem::remove(alias);
		const auto manifest = Json::parse(published);
		const auto checkpoint = directory / manifest.at("Checkpoint").get<std::string>();
		const auto checkpointBytes = Aster::ReadTextFile(checkpoint, 64ULL * 1024 * 1024);
		const auto outside = root / "Outside.aster";
		Aster::WriteTextFileAtomically(outside, checkpointBytes);
		std::filesystem::remove(checkpoint);
		std::error_code symlinkError;
		std::filesystem::create_symlink(outside, checkpoint, symlinkError);
#ifndef _WIN32
		Check(!symlinkError, "Unix checkpoint symlink test must execute");
#endif
		if (!symlinkError)
		{
			Rejects([&] { store.Checkpoint(document); }, "Linked checkpoint rejects publication");
			Rejects([&] { store.Clear(); }, "Linked checkpoint rejects cleanup");
			Check(Aster::ReadTextFile(outside, 64ULL * 1024 * 1024) == checkpointBytes &&
					  Aster::ReadTextFile(manifestPath, 64 * 1024) == published,
				  "Rejected linked operations preserve outside bytes and publication");
			std::filesystem::remove(checkpoint);
		}
		Aster::WriteTextFileAtomically(checkpoint, checkpointBytes);
		Aster::WriteTextFileAtomically(manifestPath, std::string(65537, 'x'));
		Rejects([&] { store.Checkpoint(document); }, "Oversized manifest cannot be read or replaced");
		store.Clear();
		Check(store.List().empty(), "Explicit cleanup can discard oversized regular checkpoint data");
	}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		Check(argc == 3, "Expected mode and path");
		if (std::string_view(argv[1]) == "hash")
		{
			std::cout << Aster::ComputeSha256(Aster::ReadTextFile(argv[2], 64ULL * 1024 * 1024)) << '\n';
			return 0;
		}
		Check(std::string_view(argv[1]) == "unit", "Unknown mode");
		Check(Aster::ComputeSha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
			  "SHA empty vector");
		Check(Aster::ComputeSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
			  "SHA abc vector");
		const Path root = argv[2];
		TestStore(root / "Store");
		TestStorageFailures(root / "StorageFailures");
		TestCommands(root);
		std::cout << "Recovery storage, validation, budgets and document lifecycle passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
