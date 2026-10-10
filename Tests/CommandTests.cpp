#include <Aster/Editor/CommandProcessor.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class TemporaryProject
	{
	  public:
		TemporaryProject()
		{
			const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
			Path = std::filesystem::temp_directory_path() / ("AsterCommands-" + std::to_string(tick));
			std::filesystem::create_directories(Path);
		}
		~TemporaryProject()
		{
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
		std::filesystem::path Path;
	};
} // namespace

void RunCommandTests()
{
	TemporaryProject project;
	Aster::CommandProcessor editor(project.Path);
	auto response = editor.Execute({{"id", "request-1"}, {"command", "entity.create"}, {"name", "Ball"}});
	Check(response.at("ok") && response.at("id") == "request-1", "Request correlation and create");
	const uint64_t ballID = response.at("result").at("entity");
	const auto before = editor.GetScene().Serialize();

	response = editor.Execute({{"command", "entity.patch"},
							   {"entity", ballID},
							   {"patch", {{"Transform", {{"Translation", {0.0, 10.0, 0.0}}}}}}});
	Check(response.at("ok"), "Partial component patch");
	Check(editor.GetScene().Get(editor.GetScene().FindByID(ballID)).Transform.Translation.y == 10.0f,
		  "Patch changed position");
	Check(editor.Execute({{"command", "history.undo"}}).at("ok"), "Undo patch");
	Check(editor.GetScene().Serialize() == before, "Undo restored exact authored state");
	Check(editor.Execute({{"command", "history.redo"}}).at("ok"), "Redo patch");
	const auto after = editor.GetScene().Serialize();
	const auto retainedHandle = editor.GetScene().FindByID(ballID);

	response = editor.Execute(
		{{"command", "entity.patch"}, {"entity", ballID}, {"patch", {{"Transform", {{"Scale", {0.0, 1.0, 1.0}}}}}}});
	Check(!response.at("ok").get<bool>(), "Invalid transform rejected");
	Check(editor.GetScene().Serialize() == after, "Failed edit is atomic");
	Check(!editor.Execute({{"command", "entity.parent"}, {"entity", ballID}, {"parent", ballID}}).at("ok").get<bool>(),
		  "Cycle rejected");
	Check(!editor.Execute({{"command", "scene.save"}, {"path", "../escape.aster"}}).at("ok").get<bool>(),
		  "Project traversal rejected");
	Check(!editor.Execute({{"command", "bogus"}}).at("ok").get<bool>(), "Unknown command rejected");
	Check(!editor.Execute(nlohmann::json::array()).at("ok").get<bool>(), "Nonobject request rejected");
	Check(!editor.Execute({{"command", "entity.destroy"}, {"entity", 1.5}}).at("ok").get<bool>(),
		  "Fractional entity ID rejected");
	Check(!editor.Execute({{"command", "entity.destroy"}, {"entity", -1}}).at("ok").get<bool>(),
		  "Negative entity ID rejected");
	Check(editor.GetScene().IsAlive(retainedHandle), "Rejected commands preserve live entity handles");
	std::error_code symlinkError;
	std::filesystem::create_directory_symlink(project.Path.parent_path(), project.Path / "Outside", symlinkError);
	if (!symlinkError)
	{
		Check(!editor.Execute({{"command", "scene.save"}, {"path", "Outside/Escape.aster"}}).at("ok").get<bool>(),
			  "Symlink save escape rejected");
		Check(!editor.Execute({{"command", "scene.load"}, {"path", "Outside/Escape.aster"}}).at("ok").get<bool>(),
			  "Symlink load escape rejected");
	}

	Check(editor.Execute({{"command", "scene.save"}, {"path", "Test.aster"}}).at("ok"), "Save scene");
	Check(editor.Execute({{"command", "scene.new"}, {"name", "Empty"}, {"discardChanges", true}}).at("ok"),
		  "New scene");
	Check(editor.GetScene().Size() == 0, "New scene empty");
	Check(editor.Execute({{"command", "scene.load"}, {"path", "Test.aster"}, {"discardChanges", true}}).at("ok"),
		  "Load scene");
	Check(editor.GetScene().Serialize() == after, "Load persisted scene");

	Check(!editor.Execute({{"command", "simulation.start"}, {"audio", "unknown"}}).at("ok").get<bool>(),
		  "Invalid audio mode rejected before play");
	Check(!editor.IsPlaying() && editor.GetScene().Serialize() == after,
		  "Invalid play settings preserve authored state");
	Check(editor.Execute({{"command", "simulation.start"}, {"audio", "offline"}}).at("ok"), "Start play");
	Check(editor.IsPlaying(), "Play mode active");
	Check(editor.Execute({{"command", "input.set"}, {"input", {{"KeysDown", {"A"}}, {"KeysPressed", {"A"}}}}}).at("ok"),
		  "Input snapshots accepted during play");
	Check(!editor.Execute({{"command", "input.set"}, {"input", {{"Unknown", true}}}}).at("ok").get<bool>(),
		  "Unknown input fields rejected");
	Check(!editor.Execute({{"command", "input.set"}, {"input", {{"MouseDown", {true}}}}}).at("ok").get<bool>(),
		  "Malformed mouse array rejected");
	Check(!editor.Execute({{"command", "input.set"}, {"input", {{"MouseDelta", {true, 1}}}}}).at("ok").get<bool>(),
		  "Nonnumeric mouse delta rejected");
	Check(!editor.Execute({{"command", "simulation.step"}, {"steps", 1.5}}).at("ok").get<bool>(),
		  "Fractional step count rejected");
	Check(!editor.Execute({{"command", "entity.create"}}).at("ok").get<bool>(), "Authoring blocked during play");
	Check(editor.Execute({{"command", "simulation.step"}, {"steps", 3}}).at("ok"), "Advance play");
	Check(editor.Execute({{"command", "simulation.stop"}}).at("ok"), "Stop play");
	Check(editor.GetScene().Serialize() == after, "Stop restores authored state");
	Check(!editor.Execute({{"command", "simulation.step"}}).at("ok").get<bool>(), "Cannot step stopped scene");

	response = editor.Execute({{"command", "prefab.spawn"}, {"path", "Test.aster"}, {"root", ballID}});
	Check(response.at("ok") && editor.GetScene().Size() == 2, "Prefab spawn creates entity");
	Check(response.at("result").at("entity") != ballID, "Prefab gets distinct identity");
	Check(editor.Execute({{"command", "entity.destroy"}, {"entity", ballID}}).at("ok"), "Destroy entity");
	Check(!editor.Execute({{"command", "entity.destroy"}, {"entity", ballID}}).at("ok").get<bool>(),
		  "Repeated destruction rejected");
	Check(editor.Execute({{"command", "history.undo"}}).at("ok"), "Undo destroy");
	Check(editor.GetScene().Size() == 2, "Undo restored entity");
	Check(!editor.Execute({{"command", "input.set"}, {"input", nlohmann::json::object()}}).at("ok").get<bool>(),
		  "Stopped editor rejects gameplay input");

	Aster::CommandProcessor transactions(project.Path);
	Check(transactions.Execute({{"command", "history.begin"}}).at("ok"), "Begin edit transaction");
	Check(!transactions.Execute({{"command", "history.begin"}}).at("ok").get<bool>(), "Nested edit rejected");
	Check(transactions.Execute({{"command", "entity.create"}, {"name", "First"}}).at("ok"), "First grouped edit");
	Check(transactions.Execute({{"command", "entity.create"}, {"name", "Second"}}).at("ok"), "Second grouped edit");
	Check(!transactions.Execute({{"command", "simulation.start"}}).at("ok").get<bool>(),
		  "Play requires completed transaction");
	Check(!transactions.Execute({{"command", "history.undo"}}).at("ok").get<bool>(),
		  "Undo requires completed transaction");
	Check(transactions.Execute({{"command", "history.commit"}}).at("ok"), "Commit grouped edit");
	Check(transactions.Execute({{"command", "history.undo"}}).at("ok"), "Undo grouped edit");
	Check(transactions.GetScene().Size() == 0, "One undo restores entire grouped edit");
	Check(transactions.Execute({{"command", "history.redo"}}).at("ok"), "Redo grouped edit");
	Check(transactions.GetScene().Size() == 2, "Redo restores entire grouped edit");
	const auto committed = transactions.GetScene().Serialize();
	Check(transactions.Execute({{"command", "history.begin"}}).at("ok"), "Begin cancellable edit");
	Check(transactions.Execute({{"command", "entity.destroy"}, {"entity", 1}}).at("ok"), "Temporary deletion");
	Check(transactions.Execute({{"command", "history.cancel"}}).at("ok"), "Cancel grouped edit");
	Check(transactions.GetScene().Serialize() == committed, "Cancellation restores authored state");
	Check(
		transactions
			.Execute({{"command", "scene.environment"}, {"environment", {{"Path", "Studio.hdr"}, {"Intensity", 2.0}}}})
			.at("ok"),
		"Author environment selection");
	Check(transactions.GetScene().GetEnvironment().Intensity == 2.0f, "Environment authored through shared command");
	Check(!transactions.Execute({{"command", "scene.environment"}, {"environment", {{"Path", "../Outside.hdr"}}}})
			   .at("ok")
			   .get<bool>(),
		  "Invalid environment rejected");
	Check(transactions.GetScene().GetEnvironment().Path == "Studio.hdr",
		  "Failed environment edit preserves previous value");
}
