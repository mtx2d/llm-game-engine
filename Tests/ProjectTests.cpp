#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/CommandProcessor.h>
#include <Aster/Project/Project.h>
#include <Aster/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
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

	class TemporaryDirectory
	{
	  public:
		TemporaryDirectory()
		{
			Path = std::filesystem::temp_directory_path() /
				   ("AsterProject-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			Check(std::filesystem::create_directory(Path), "Create test directory exclusively");
			Path = std::filesystem::canonical(Path);
		}
		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
		std::filesystem::path Path;
	};

	void TestJsonFiles(const std::filesystem::path& root)
	{
		const auto path = root / "File.json";
		Aster::WriteTextFileAtomically(path, R"({"name":"first","value":1})");
		Check(Aster::ReadJsonFile(path, 1024).at("value") == 1, "Atomic file is readable");
		Aster::WriteTextFileAtomically(path, R"({"name":"second","value":2})");
		Check(Aster::ReadJsonFile(path, 1024).at("value") == 2, "Atomic file replaces existing contents");
		const auto expected = Aster::ReadTextFile(path, 1024);
		Rejects([&] { Aster::WriteTextFileConditionally(path, "new", std::nullopt); },
				"Exclusive publication cannot replace an existing file");
		Rejects([&] { Aster::WriteTextFileConditionally(path, "new", "different"); },
				"Conditional publication rejects changed bytes");
		Check(Aster::ReadTextFile(path, 1024) == expected, "Rejected publications preserve exact bytes");
		Aster::WriteTextFileConditionally(path, "", expected);
		Aster::WriteTextFileConditionally(path, expected, "");
		Check(Aster::ReadTextFile(path, 1024) == expected, "An empty expected file differs from an absent file");
		Aster::WriteTextFileConditionally(root / "Exclusive.json", expected, std::nullopt);
		Check(Aster::ReadTextFile(root / "Exclusive.json", 1024) == expected,
			  "Exclusive output publishes complete bytes");
		Rejects([&] { (void)Aster::ReadJsonFile(path, 2); }, "Oversized JSON rejected");
		Rejects([&] { (void)Aster::ReadJsonFile(path, 0); }, "Zero read limit rejected");
		Rejects([&] { (void)Aster::ReadJsonFile(root, 1024); }, "Directory input rejected");
		for (const auto* invalid : {"{} {}", "{", R"({"a":1,"\u0061":2})", R"({"array":[{"x":1,"x":2}]})"})
		{
			Aster::WriteTextFileAtomically(path, invalid);
			Rejects([&] { (void)Aster::ReadJsonFile(path, 1024); }, "Ambiguous or malformed JSON rejected");
		}
		Aster::WriteTextFileAtomically(path, R"({"array":[{"x":1},{"x":2}]})");
		Check(Aster::ReadJsonFile(path, 1024).at("array").size() == 2, "Keys may repeat in distinct objects");
		Aster::WriteTextFileAtomically(path, std::string(24, '[') + "0" + std::string(24, ']'));
		Rejects([&] { (void)Aster::ReadJsonFile(path, 1024, 8); }, "Deep JSON rejected while parsing");
		const auto directory = root / "CannotReplace";
		std::filesystem::create_directory(directory);
		Aster::WriteTextFileAtomically(directory / "Keep.json", "{\"keep\":true}");
		Rejects([&] { Aster::WriteTextFileAtomically(directory, "replacement"); }, "Directory replacement fails");
		Check(Aster::ReadJsonFile(directory / "Keep.json", 1024).at("keep"), "Failed publish preserves destination");
		for (const auto& entry : std::filesystem::directory_iterator(root))
		{
			Check(entry.path().filename().string().find(".aster-tmp-") == std::string::npos,
				  "Failed publish removes owned temporary file");
		}
		std::error_code error;
		std::filesystem::create_symlink(directory / "Keep.json", root / "Link.json", error);
		if (!error)
		{
			Rejects([&] { Aster::WriteTextFileAtomically(root / "Link.json", "{}"); }, "Atomic output rejects symlink");
			Check(Aster::ReadJsonFile(directory / "Keep.json", 1024).at("keep"), "Symlink target unchanged");
		}
	}

	void TestProjects(const std::filesystem::path& root)
	{
		auto project = Aster::Project::Create(root / "Game With Spaces", "First Game");
		const auto path = project.GetFilePath();
		Check(path == root / "Game With Spaces/Project.asterproj", "Project file resides in new directory");
		Check(project.GetAssetDirectory() == root / "Game With Spaces/Assets",
			  "Assets resolved without cwd dependence");
		const auto document = project.Serialize();
		Check(Aster::Project::Load(path).Serialize() == document, "Project round trip preserves portable config");
		const auto scene = Aster::Scene::Load(project.ResolveAssetPath(project.GetConfig().StartScene));
		Check(scene.Size() == 1 && scene.Get(scene.Entities().front()).Camera.has_value(),
			  "New project has a primary camera scene");
		Rejects([&] { (void)Aster::Project::Create(root / "Game With Spaces", "Overwrite"); },
				"Existing project cannot be overwritten");
		Check(Aster::Project::Load(path).Serialize() == document, "Rejected creation preserves existing project");
		Rejects([&] { (void)Aster::Project::Create(root / "Invalid", ""); }, "Empty project name rejected");
		Check(!std::filesystem::exists(root / "Invalid"), "Failed creation publishes nothing");
		Rejects([&] { (void)Aster::Project::Create(root / "Missing/Invalid", "Name"); }, "Missing parent rejected");
		for (const auto* invalid : {"../Escape.aster", "/Escape.aster", "C:/Escape.aster", ""})
		{
			Rejects([&] { (void)project.ResolveAssetPath(invalid); }, "Invalid relative asset path rejected");
		}
		for (const auto* field : {"Name", "AssetDirectory", "StartScene"})
		{
			auto invalid = document;
			invalid[field] = false;
			Aster::WriteTextFileAtomically(path, invalid.dump());
			Rejects([&] { (void)Aster::Project::Load(path); }, "Wrong field type rejected");
		}
		for (const auto& version : {nlohmann::json(2), nlohmann::json(1.0), nlohmann::json(true)})
		{
			auto invalid = document;
			invalid["Version"] = version;
			Aster::WriteTextFileAtomically(path, invalid.dump());
			Rejects([&] { (void)Aster::Project::Load(path); }, "Unsupported or noninteger version rejected");
		}
		auto invalid = document;
		invalid["Unknown"] = 1;
		Aster::WriteTextFileAtomically(path, invalid.dump());
		Rejects([&] { (void)Aster::Project::Load(path); }, "Unknown config field rejected");
		Aster::WriteTextFileAtomically(path, std::string(65537, ' '));
		Rejects([&] { (void)Aster::Project::Load(path); }, "Project size bounded");
		Aster::WriteTextFileAtomically(path, R"({"Version":1,"Version":1})");
		Rejects([&] { (void)Aster::Project::Load(path); }, "Duplicate project fields rejected");
		Aster::WriteTextFileAtomically(path, document.dump());
		auto config = project.GetConfig();
		config.StartScene = "Scenes/Missing.aster";
		Rejects([&] { project.UpdateConfig(config); }, "Missing startup scene rejected");
		Check(project.Serialize() == document && Aster::Project::Load(path).Serialize() == document,
			  "Rejected update preserves in-memory and persisted config");
		config = project.GetConfig();
		config.AssetDirectory = "../Outside";
		Rejects([&] { project.UpdateConfig(config); }, "Asset directory escape rejected");
		config = project.GetConfig();
		config.Name = std::string(129, 'x');
		Rejects([&] { project.UpdateConfig(config); }, "Oversized project name rejected");
		config.Name = "Control\nName";
		Rejects([&] { project.UpdateConfig(config); }, "Control characters rejected in project names");
		config.Name = std::string(1, static_cast<char>(0xff));
		Rejects([&] { project.UpdateConfig(config); }, "Invalid UTF-8 rejected before publishing config");
		config = project.GetConfig();
		config.Name = "UTF-8 \xe9\x9b\xaa";
		project.UpdateConfig(config);
		Check(Aster::Project::Load(path).GetConfig().Name == config.Name, "UTF-8 project name round trips");
		config.Name = "Updated Game";
		project.UpdateConfig(config);
		Check(Aster::Project::Load(path).GetConfig().Name == "Updated Game", "Valid update is persisted");
		const auto before = project.Serialize();
		std::filesystem::remove(path);
		std::filesystem::create_directory(path);
		Aster::WriteTextFileAtomically(path / "Keep.json", "{}");
		config.Name = "Failed Update";
		Rejects([&] { project.UpdateConfig(config); }, "Unwritable project target reports failure");
		Check(project.Serialize() == before && std::filesystem::is_regular_file(path / "Keep.json"),
			  "Failed persistence preserves config and existing destination contents");
		std::filesystem::remove_all(path);
		Aster::WriteTextFileAtomically(path, before.dump());
		std::error_code error;
		std::filesystem::create_directory_symlink(root, project.GetAssetDirectory() / "Outside", error);
		if (!error)
		{
			Rejects([&] { (void)project.ResolveAssetPath("Outside/File.aster"); }, "Symlink asset escape rejected");
			config = project.GetConfig();
			config.AssetDirectory = "Assets/Outside";
			Rejects([&] { project.UpdateConfig(config); }, "Symlink asset directory escape rejected");
		}
		error.clear();
		std::filesystem::create_symlink(path, root / "Link.asterproj", error);
		if (!error)
		{
			Rejects([&] { (void)Aster::Project::Load(root / "Link.asterproj"); }, "Symlink manifest rejected");
			Rejects([&] { Aster::CommandProcessor editor(root / "Link.asterproj"); },
					"Editor does not canonicalize away manifest symlink check");
		}
		for (const auto& entry : std::filesystem::directory_iterator(root))
		{
			Check(!entry.path().filename().string().starts_with(".aster-project-"),
				  "Failed creation removes staging directory");
		}
	}

	void TestSaveConflicts(const std::filesystem::path& root)
	{
		const auto project = Aster::Project::Create(root / "Conflicts", "Conflicts");
		const auto path = project.GetAssetDirectory() / "Scenes/Main.aster";
		const auto original = Aster::ReadTextFile(path, 64 * 1024);
		Aster::CommandProcessor first(project.GetFilePath());
		Aster::CommandProcessor second(project.GetFilePath());
		Check(first.Execute({{"command", "entity.create"}, {"name", "First editor"}}).at("ok"), "First edit");
		Check(second.Execute({{"command", "entity.create"}, {"name", "Second editor"}}).at("ok"), "Second edit");
		Check(first.Execute({{"command", "scene.save"}, {"path", "Scenes/Main.aster"}}).at("ok"), "First save");
		const auto firstSaved = Aster::ReadTextFile(path, 64 * 1024);
		Aster::WriteTextFileAtomically(path, firstSaved + "\n");
		const auto rejectedExport = first.Execute({{"command", "project.export"}});
		Check(!rejectedExport.at("ok").get<bool>() &&
				  rejectedExport.at("error").get<std::string>().find("conflict") != std::string::npos &&
				  !first.HasUnsavedChanges(),
			  "Export checks the clean document's disk baseline before staging");
		Aster::WriteTextFileAtomically(path, firstSaved);
		const auto secondScene = second.GetScene().Serialize();
		const auto stable = second.GetScene().Entities().front();
		Check(!second.Execute({{"command", "scene.save"}, {"path", "Scenes/Main.aster"}}).at("ok").get<bool>(),
			  "Another editor's save is a conflict");
		Check(Aster::ReadTextFile(path, 64 * 1024) == firstSaved && second.HasUnsavedChanges() &&
				  second.GetScene().Serialize() == secondScene && second.GetScene().IsAlive(stable) &&
				  second.GetScenePath() == "Scenes/Main.aster",
			  "Conflict preserves disk, memory, handles, document identity and dirtiness");
		Check(second.Execute({{"command", "history.undo"}}).at("ok") && !second.HasUnsavedChanges(),
			  "Conflict preserves Undo and clean baseline");
		Check(second.Execute({{"command", "history.redo"}}).at("ok") && second.HasUnsavedChanges(),
			  "Conflict preserves Redo");
		Aster::WriteTextFileAtomically(project.GetAssetDirectory() / "Other.aster", "keep exact bytes");
		Check(!second.Execute({{"command", "scene.save"}, {"path", "Other.aster"}}).at("ok").get<bool>(),
			  "Save As cannot silently replace another document");
		Check(Aster::ReadTextFile(project.GetAssetDirectory() / "Other.aster", 1024) == "keep exact bytes" &&
				  second.GetScenePath() == "Scenes/Main.aster" && second.HasUnsavedChanges(),
			  "Save As collision preserves destination and source identity");
		Check(second.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok") &&
				  !second.HasUnsavedChanges(),
			  "Save As recovers in-memory work after a conflict");
		const auto recoveredPath = project.GetAssetDirectory() / "Scenes/Recovered.aster";
		const auto recoveredBytes = Aster::ReadTextFile(recoveredPath, 64 * 1024);
		const auto timestamp = std::filesystem::last_write_time(recoveredPath);
		auto changed = recoveredBytes;
		const auto nameOffset = changed.find("Second editor");
		Check(nameOffset != std::string::npos, "Recovered scene contains the pending editor's name");
		changed[nameOffset] = 's';
		Aster::WriteTextFileAtomically(recoveredPath, changed);
		std::filesystem::last_write_time(recoveredPath, timestamp);
		Check(!second.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok").get<bool>(),
			  "Same-size, same-timestamp modification is detected by contents");
		Check(Aster::ReadTextFile(recoveredPath, 64 * 1024) == changed, "Conflict leaves external contents intact");
		Aster::WriteTextFileAtomically(recoveredPath, recoveredBytes + "\n");
		Check(!second.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok").get<bool>(),
			  "Whitespace-only external changes are also conflicts");
		std::filesystem::remove(recoveredPath);
		Check(!second.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok").get<bool>() &&
				  !std::filesystem::exists(recoveredPath),
			  "Deleted document cannot silently be recreated");
		Aster::WriteTextFileAtomically(recoveredPath, recoveredBytes);
		Check(second.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok"),
			  "Restoring exact baseline bytes allows retry");
		Check(first.Execute({{"command", "scene.load"}, {"path", "Scenes/Recovered.aster"}}).at("ok"),
			  "Reload accepts an external version explicitly");
		Check(first.Execute({{"command", "scene.save"}, {"path", "Scenes/Recovered.aster"}}).at("ok"),
			  "Reload establishes the new file baseline");
		Check(original != firstSaved, "Fixture made a real persisted change");
		Aster::SceneDocumentFile file;
		auto smallScene = file.Load(recoveredPath);
		Aster::WriteTextFileAtomically(root / "Malformed.aster", "{");
		Rejects([&] { (void)file.Load(root / "Malformed.aster"); },
				"Malformed load rejects before changing file baseline");
		Aster::Scene oversized("Oversized");
		for (int index = 0; index < 3000; ++index)
		{
			// Valid names expand sixfold when JSON-escaped, beyond the load limit.
			(void)oversized.CreateEntity(std::string(4096, '\x01'));
		}
		const auto beforeOversized = Aster::ReadTextFile(recoveredPath, 64 * 1024);
		bool sizeRejected = false;
		try
		{
			file.Save(oversized, recoveredPath);
		}
		catch (const std::invalid_argument& error)
		{
			sizeRejected = std::string(error.what()).find("64 MiB") != std::string::npos;
		}
		Check(sizeRejected, "Oversized save reports its size limit before publication");
		sizeRejected = false;
		try
		{
			oversized.Save(recoveredPath);
		}
		catch (const std::invalid_argument& error)
		{
			sizeRejected = std::string(error.what()).find("64 MiB") != std::string::npos;
		}
		Check(sizeRejected, "Core scene persistence enforces the same reloadable size limit");
		Check(Aster::ReadTextFile(recoveredPath, 64 * 1024) == beforeOversized,
			  "Oversized save preserves existing contents");
		file.Save(smallScene, recoveredPath);
		Check(Aster::Scene::Load(recoveredPath).Serialize() == smallScene.Serialize(),
			  "Failed load and oversized save preserve the original file baseline for retry");
	}

	void TestSessionClose(const std::filesystem::path& root)
	{
		const auto project = Aster::Project::Create(root / "Close", "Close");
		Aster::CommandProcessor editor(project.GetFilePath());
		Check(!editor.Execute({{"command", "session.close"}, {"discardChanges", "true"}}).at("ok").get<bool>() &&
				  !editor.IsClosed(),
			  "Discard must be a boolean even on a clean document");
		Check(editor.Execute({{"command", "entity.create"}}).at("ok"), "Make dirty document");
		const auto stable = editor.GetScene().Entities().front();
		Check(!editor.Execute({{"command", "session.close"}}).at("ok").get<bool>() && !editor.IsClosed() &&
				  editor.HasUnsavedChanges() && editor.GetScene().IsAlive(stable),
			  "Dirty close rejection preserves authored state and session");
		Check(editor.Execute({{"command", "history.begin"}}).at("ok"), "Begin edit before closing");
		Check(!editor.Execute({{"command", "session.close"}, {"discardChanges", true}}).at("ok").get<bool>(),
			  "Unfinished edits cannot be closed even with discard");
		Check(editor.Execute({{"command", "history.cancel"}}).at("ok"), "Finish closing edit");
		Check(editor.Execute({{"command", "simulation.start"}, {"audio", "offline"}}).at("ok"), "Play before closing");
		Check(!editor.Execute({{"command", "session.close"}, {"discardChanges", true}}).at("ok").get<bool>() &&
				  editor.IsPlaying() && !editor.IsClosed(),
			  "Close requires explicit simulation teardown");
		Check(editor.Execute({{"command", "simulation.stop"}}).at("ok"), "Stop before closing");
		Check(editor.Execute({{"command", "session.close"}, {"discardChanges", true}}).at("ok") && editor.IsClosed(),
			  "Explicit discard closes session");
		Check(!editor.Execute({{"command", "entity.create"}}).at("ok").get<bool>(),
			  "Closed session rejects further edits");
		Aster::CommandProcessor clean(project.GetFilePath());
		Check(clean.Execute({{"command", "session.close"}}).at("ok") && clean.IsClosed(),
			  "Clean document closes directly");
	}

	void TestDocumentLifecycle(const std::filesystem::path& root)
	{
		const auto first = Aster::Project::Create(root / "First", "First");
		const auto second = Aster::Project::Create(root / "Second", "Second");
		Aster::CommandProcessor editor(first.GetFilePath());
		Check(!editor.HasUnsavedChanges() && editor.GetScenePath() == "Scenes/Main.aster",
			  "Startup scene is a clean document");
		Check(editor.Execute({{"command", "project.get"}}).at("result").at("config").at("Name") == "First",
			  "Active project exposed to automation");
		const auto camera = editor.GetScene().Entities().front();
		Check(editor.Execute({{"command", "entity.create"}, {"name", "Unsaved"}}).at("ok"), "Edit project scene");
		const auto authored = editor.GetScene().Serialize();
		Check(editor.HasUnsavedChanges(), "Authoring dirties document");
		Check(!editor.Execute({{"command", "project.create"},
							   {"path", (root / "MustNotCreate").string()},
							   {"name", "Rejected"}})
					  .at("ok")
					  .get<bool>() &&
				  !std::filesystem::exists(root / "MustNotCreate"),
			  "Dirty project creation is rejected before filesystem mutation");
		const auto exportResponse = editor.Execute({{"command", "project.export"}});
		Check(!exportResponse.at("ok").get<bool>() &&
				  exportResponse.at("error").get<std::string>().find("save") != std::string::npos,
			  "Export rejects dirty state before copying stale files");
		Check(!editor.Execute({{"command", "scene.new"}}).at("ok").get<bool>(),
			  "New scene cannot silently discard changes");
		Check(!editor.Execute({{"command", "scene.load"}, {"path", "Scenes/Main.aster"}}).at("ok").get<bool>(),
			  "Load cannot silently discard changes");
		Check(!editor.Execute({{"command", "project.open"}, {"path", second.GetFilePath().string()}})
				   .at("ok")
				   .get<bool>(),
			  "Project switch cannot silently discard changes");
		Check(!editor.Execute({{"command", "scene.save"}, {"path", "Missing/File.aster"}}).at("ok").get<bool>(),
			  "Save failure reported");
		Check(editor.HasUnsavedChanges() && editor.GetScene().Serialize() == authored &&
				  editor.GetScene().IsAlive(camera),
			  "Rejected commands preserve document, dirtiness and handles");
		Check(editor.Execute({{"command", "history.undo"}}).at("ok") && !editor.HasUnsavedChanges(),
			  "Undo to saved content clears dirty state");
		Check(editor.Execute({{"command", "history.redo"}}).at("ok") && editor.HasUnsavedChanges(),
			  "Redo away from saved content dirties document");
		Check(editor.Execute({{"command", "history.begin"}}).at("ok"), "Begin grouped edit");
		Check(!editor.Execute({{"command", "scene.save"}, {"path", "Scenes/Main.aster"}}).at("ok").get<bool>(),
			  "Active transaction cannot be persisted halfway through");
		Check(!editor
				   .Execute(
					   {{"command", "project.open"}, {"path", second.GetFilePath().string()}, {"discardChanges", true}})
				   .at("ok")
				   .get<bool>(),
			  "Project switch rejects active transaction");
		Check(editor.Execute({{"command", "history.cancel"}}).at("ok"), "Cancel grouped edit");
		Aster::WriteTextFileAtomically(
			first.GetAssetDirectory() / "Mutation.lua",
			"return {OnCreate=function(self, entity) engine.set_name(entity, 'Runtime only') end}");
		Check(editor
				  .Execute({{"command", "entity.patch"},
							{"entity", 2},
							{"patch", {{"Script", {{"Path", "Mutation.lua"}, {"Enabled", true}}}}}})
				  .at("ok"),
			  "Attach a mutating script");
		Check(editor.Execute({{"command", "scene.save"}, {"path", "Scenes/Saved.aster"}}).at("ok") &&
				  !editor.HasUnsavedChanges(),
			  "Successful Save As records clean baseline and path");
		Check(editor.GetScenePath() == "Scenes/Saved.aster", "Save As updates document identity");
		Check(editor.Execute({{"command", "simulation.start"}, {"audio", "offline"}}).at("ok"), "Play project scene");
		Check(editor.GetScene().Get(editor.GetScene().FindByID(2)).Name == "Runtime only",
			  "Play actually changes runtime content");
		Check(!editor.HasUnsavedChanges(), "Play content does not dirty authored document");
		Check(!editor
				   .Execute(
					   {{"command", "project.open"}, {"path", second.GetFilePath().string()}, {"discardChanges", true}})
				   .at("ok")
				   .get<bool>(),
			  "Project switch is blocked during play");
		Check(editor.Execute({{"command", "simulation.stop"}}).at("ok") && !editor.HasUnsavedChanges(),
			  "Stop restores clean authored state");
		const auto stable = editor.GetScene().Entities().front();
		Check(!editor.Execute({{"command", "project.open"}, {"path", (root / "Missing.asterproj").string()}})
				   .at("ok")
				   .get<bool>(),
			  "Invalid project switch rejected");
		Check(editor.GetScene().IsAlive(stable) && editor.GetAssetRoot() == first.GetAssetDirectory(),
			  "Failed switch preserves scene handles and root");
		Check(editor.Execute({{"command", "project.open"}, {"path", second.GetFilePath().string()}}).at("ok"),
			  "Open another project");
		Check(editor.GetAssetRoot() == second.GetAssetDirectory() && editor.GetScene().Size() == 1 &&
				  !editor.HasUnsavedChanges(),
			  "Switch publishes new root and startup scene together");
		Check(!editor.Execute({{"command", "history.undo"}}).at("ok").get<bool>(),
			  "Undo history cannot cross project boundaries");
		Check(editor.Execute({{"command", "scene.new"}, {"name", "New"}}).at("ok") && editor.HasUnsavedChanges(),
			  "New untitled document needs saving");
		Check(editor.Execute({{"command", "scene.load"}, {"path", "Scenes/Main.aster"}, {"discardChanges", true}})
					  .at("ok") &&
				  !editor.HasUnsavedChanges(),
			  "Explicit discard loads clean document");
		Check(!editor.Execute({{"command", "history.undo"}}).at("ok").get<bool>(),
			  "Undo cannot resurrect previous document");
	}
} // namespace

void RunProjectTests()
{
	TemporaryDirectory directory;
	TestJsonFiles(directory.Path);
	TestProjects(directory.Path);
	TestDocumentLifecycle(directory.Path);
	TestSaveConflicts(directory.Path);
	TestSessionClose(directory.Path);
}
