#include <Aster/Core/Hash.h>
#include <Aster/Editor/CommandProcessor.h>
#include <Aster/Editor/ProjectExporter.h>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace Aster
{
	namespace
	{
		class UnsavedChanges final : public std::logic_error
		{
		  public:
			UnsavedChanges()
				: std::logic_error("Unsaved changes: save the scene or explicitly set discardChanges to true")
			{
			}
		};

		uint64_t ReadUnsigned(const nlohmann::json& value, uint64_t maximum)
		{
			if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
			{
				throw std::invalid_argument("Expected a nonnegative integer");
			}
			const auto result = value.get<uint64_t>();
			if (result > maximum)
			{
				throw std::invalid_argument("Integer is outside the supported range");
			}
			return result;
		}

		uint64_t ReadEntityID(const nlohmann::json& request, const char* field)
		{
			return ReadUnsigned(request.at(field), static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
		}
	} // namespace

	CommandProcessor::CommandProcessor(std::filesystem::path projectRoot, SimulationSettings simulationSettings,
									   bool allowStartupRepair)
		: m_ProjectRoot(std::move(projectRoot)), m_SimulationSettings(simulationSettings)
	{
		m_SavedScene = m_Scene.Serialize();
		if (m_ProjectRoot.extension() == ".asterproj" || std::filesystem::is_regular_file(m_ProjectRoot))
		{
			OpenProject(allowStartupRepair ? Project::LoadForRepair(m_ProjectRoot) : Project::Load(m_ProjectRoot),
						allowStartupRepair);
			return;
		}
		m_ProjectRoot = std::filesystem::weakly_canonical(m_ProjectRoot);
		if (!std::filesystem::is_directory(m_ProjectRoot))
		{
			throw std::invalid_argument("Project root must be an existing directory");
		}
	}

	bool CommandProcessor::HasUnsavedChanges() const
	{
		return (IsPlaying() ? m_PlaySnapshot : m_Scene.Serialize()) != m_SavedScene;
	}

	void CommandProcessor::EnableRecovery()
	{
		if (!m_Recovery)
		{
			m_Recovery = std::make_unique<RecoveryStore>(m_ProjectRoot);
			m_RecoveryPending = true;
		}
	}

	void CommandProcessor::CheckpointRecovery()
	{
		EnableRecovery();
		if (m_EditTransaction)
		{
			throw std::logic_error("Finish the current edit before checkpointing recovery");
		}
		if (!HasUnsavedChanges())
		{
			m_Recovery->Clear();
		}
		else
		{
			RecoveryDocument document;
			document.Scene = IsPlaying() ? m_PlaySnapshot : m_Scene.Serialize();
			document.Path = m_ScenePath;
			if (document.Path)
			{
				std::replace(document.Path->begin(), document.Path->end(), '\\', '/');
				document.SourceDigest = ComputeSha256(m_DocumentFile.GetContents());
			}
			document.SavedDigest = ComputeSha256(m_SavedScene.dump());
			m_Recovery->Checkpoint(document);
		}
		m_RecoveryPending = false;
	}

	std::optional<std::string> CommandProcessor::UpdateRecovery(bool force)
	{
		if (!m_Recovery || !m_RecoveryPending || m_IsClosed || m_EditTransaction)
		{
			return std::nullopt;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!force && now < m_NextRecoveryCheckpoint)
		{
			return std::nullopt;
		}
		m_NextRecoveryCheckpoint = now + std::chrono::seconds(5);
		try
		{
			CheckpointRecovery();
		}
		catch (const std::exception& error)
		{
			return "Recovery checkpoint failed: " + std::string(error.what());
		}
		return std::nullopt;
	}

	nlohmann::json CommandProcessor::RestoreRecovery(const nlohmann::json& request)
	{
		RequireDocumentChange(request);
		EnableRecovery();
		auto document = m_Recovery->Load(request.at("session").get<std::string>());
		auto scene = Scene::Deserialize(document.Scene);
		SceneDocumentFile file;
		nlohmann::json saved;
		std::string warning;
		if (document.Path)
		{
			try
			{
				auto original = file.Load(ResolvePath(*document.Path));
				saved = original.Serialize();
				if (ComputeSha256(file.GetContents()) != *document.SourceDigest ||
					ComputeSha256(saved.dump()) != document.SavedDigest)
				{
					throw std::runtime_error("The original scene changed since the checkpoint");
				}
			}
			catch (const std::exception& error)
			{
				warning = "Recovered as an unsaved copy; choose a new filename. " + std::string(error.what());
				document.Path.reset();
				document.SourceDigest.reset();
				saved = nullptr;
				file = SceneDocumentFile{};
				document.SavedDigest = ComputeSha256(saved.dump());
			}
		}
		if (!document.Path)
		{
			document.SavedDigest = ComputeSha256(saved.dump());
		}
		// Protect the adopted work before publishing it to memory. The source
		// checkpoint remains available until the user explicitly discards it.
		m_Recovery->Checkpoint(document);
		m_Scene = std::move(scene);
		m_DocumentFile = std::move(file);
		m_ScenePath = std::move(document.Path);
		m_SavedScene = std::move(saved);
		m_Undo.clear();
		m_Redo.clear();
		m_RecoveryPending = false;
		return {{"restored", true},
				{"path", m_ScenePath ? nlohmann::json(*m_ScenePath) : nlohmann::json(nullptr)},
				{"warning", warning.empty() ? nlohmann::json(nullptr) : nlohmann::json(warning)}};
	}

	void CommandProcessor::RequireDocumentChange(const nlohmann::json& request) const
	{
		RequireEditing();
		if (m_EditTransaction)
		{
			throw std::logic_error("Finish the current edit before changing documents");
		}
		const bool discardChanges = request.value("discardChanges", false);
		if (HasUnsavedChanges() && !discardChanges)
		{
			throw UnsavedChanges();
		}
	}

	void CommandProcessor::EnableProjectHistory(const std::filesystem::path& stateDirectory)
	{
		m_ProjectHistory = std::make_unique<ProjectHistory>(stateDirectory);
		if (const auto error = RememberProject())
		{
			throw std::runtime_error(*error);
		}
	}

	std::optional<std::string> CommandProcessor::RememberProject()
	{
		if (m_ProjectHistory && m_Project)
		{
			try
			{
				m_ProjectHistory->Remember(m_Project->GetFilePath());
			}
			catch (const std::exception& error)
			{
				return "Project opened; recent-project history could not be updated: " + std::string(error.what());
			}
		}
		return std::nullopt;
	}

	void CommandProcessor::OpenProject(Project project, bool allowStartupRepair)
	{
		SceneDocumentFile file;
		Scene scene("Startup repair");
		std::optional<std::string> scenePath;
		std::optional<std::string> startupError;
		try
		{
			scene = file.Load(project.ResolveAssetPath(project.GetConfig().StartScene));
		}
		catch (const std::bad_alloc&)
		{
			throw;
		}
		catch (const std::exception& error)
		{
			if (!allowStartupRepair)
			{
				throw;
			}
			file = SceneDocumentFile{};
			startupError = "Startup scene needs repair: " + std::string(error.what());
		}
		if (!startupError)
		{
			scenePath = project.GetConfig().StartScene.generic_string();
		}
		auto saved = scene.Serialize();
		auto assetRoot = project.GetAssetDirectory();
		auto recovery = m_Recovery ? std::make_unique<RecoveryStore>(assetRoot) : nullptr;
		if (m_Recovery)
		{
			m_Recovery->Clear();
		}
		// All filesystem operations, validation and allocations precede publication.
		m_Project = std::move(project);
		m_StartupError = std::move(startupError);
		m_ProjectRoot = std::move(assetRoot);
		m_Scene = std::move(scene);
		m_DocumentFile = std::move(file);
		m_ScenePath = std::move(scenePath);
		m_SavedScene = std::move(saved);
		m_Undo.clear();
		m_Redo.clear();
		m_Recovery = std::move(recovery);
		m_RecoveryPending = m_Recovery != nullptr;
	}

	void CommandProcessor::UpdateSimulation(double deltaTime)
	{
		if (m_Simulation)
		{
			m_Simulation->Update(deltaTime);
		}
	}

	nlohmann::json CommandProcessor::ConfigureProject(const nlohmann::json& request)
	{
		RequireEditing();
		if (m_EditTransaction || !m_Project)
		{
			throw std::logic_error("Finish editing and open a project file before configuring it");
		}
		if (request.contains("discardChanges") && !request.at("discardChanges").is_boolean())
		{
			throw std::invalid_argument("discardChanges must be a boolean");
		}
		auto config = Project::DeserializeConfig(request.at("config"));
		auto project = m_Project->PreviewConfig(config);
		if (project.GetAssetDirectory() == m_ProjectRoot && !m_StartupError)
		{
			project.UpdateConfig(std::move(config));
			m_Project = std::move(project);
			return {{"configured", true}, {"documentChanged", false}, {"warning", nullptr}};
		}
		RequireDocumentChange(request);
		SceneDocumentFile file;
		auto scene = file.Load(project.ResolveAssetPath(project.GetConfig().StartScene));
		auto saved = scene.Serialize();
		auto scenePath = project.GetConfig().StartScene.generic_string();
		auto assetRoot = project.GetAssetDirectory();
		auto recovery = m_Recovery ? std::make_unique<RecoveryStore>(assetRoot) : nullptr;
		nlohmann::json result = {{"configured", true}, {"documentChanged", true}, {"warning", nullptr}};
		project.UpdateConfig(std::move(config));
		// Configuration persistence is the commit point. All document preparation
		// precedes it; publication below only transfers already owned state.
		auto previousRecovery = std::move(m_Recovery);
		m_Project = std::move(project);
		m_StartupError.reset();
		m_ProjectRoot = std::move(assetRoot);
		m_Scene = std::move(scene);
		m_DocumentFile = std::move(file);
		m_ScenePath = std::move(scenePath);
		m_SavedScene = std::move(saved);
		m_Undo.clear();
		m_Redo.clear();
		m_Recovery = std::move(recovery);
		m_RecoveryPending = m_Recovery != nullptr;
		if (previousRecovery)
		{
			try
			{
				previousRecovery->Clear();
			}
			catch (const std::exception& error)
			{
				result["warning"] =
					"Configuration saved; previous recovery data could not be removed: " + std::string(error.what());
			}
		}
		return result;
	}

	void CommandProcessor::SetInput(const InputSnapshot& input)
	{
		if (m_Simulation)
		{
			m_Simulation->SetInput(input);
		}
	}

	const std::vector<std::string>& CommandProcessor::GetSimulationErrors() const
	{
		static const std::vector<std::string> s_Empty;
		return m_Simulation ? m_Simulation->GetErrors() : s_Empty;
	}

	std::filesystem::path CommandProcessor::ResolvePath(const std::string& relativePath) const
	{
		Scene::ValidateAssetPath(relativePath);
		if (relativePath.empty())
		{
			throw std::invalid_argument("Project path must not be empty");
		}
		const auto resolved = std::filesystem::weakly_canonical(m_ProjectRoot / relativePath);
		auto rootIterator = m_ProjectRoot.begin();
		auto pathIterator = resolved.begin();
		for (; rootIterator != m_ProjectRoot.end(); ++rootIterator, ++pathIterator)
		{
			if (pathIterator == resolved.end() || *rootIterator != *pathIterator)
			{
				throw std::invalid_argument("Path escapes the project root");
			}
		}
		Scene::ValidateAssetPath(resolved.lexically_relative(m_ProjectRoot).generic_string());
		return resolved;
	}

	void CommandProcessor::RequireEditing() const
	{
		if (IsPlaying())
		{
			throw std::logic_error("Stop simulation before editing the scene");
		}
	}

	Entity CommandProcessor::RequireEntity(uint64_t id) const
	{
		const Entity entity = m_Scene.FindByID(id);
		if (!entity)
		{
			throw std::invalid_argument("Entity does not exist: " + std::to_string(id));
		}
		return entity;
	}

	void CommandProcessor::RecordChange(const nlohmann::json& before)
	{
		if (m_EditTransaction)
		{
			return;
		}
		if (before == m_Scene.Serialize())
		{
			return;
		}
		m_RecoveryPending = true;
		if (m_Undo.size() == 100)
		{
			m_Undo.erase(m_Undo.begin());
		}
		m_Undo.push_back(before);
		m_Redo.clear();
	}

	nlohmann::json CommandProcessor::Execute(const nlohmann::json& request)
	{
		nlohmann::json response = {{"ok", false}};
		if (request.is_object() && request.contains("id"))
		{
			response["id"] = request["id"];
		}
		try
		{
			if (m_IsClosed)
			{
				throw std::logic_error("Editor session is closed");
			}
			if (!request.is_object())
			{
				throw std::invalid_argument("Request must be a JSON object");
			}
			response["result"] = Dispatch(request);
			response["ok"] = true;
		}
		catch (const UnsavedChanges& error)
		{
			response["error"] = error.what();
			response["code"] = "unsaved_changes";
		}
		catch (const std::exception& error)
		{
			response["error"] = error.what();
		}
		return response;
	}

	nlohmann::json CommandProcessor::Dispatch(const nlohmann::json& request)
	{
		const std::string command = request.at("command").get<std::string>();
		if (command == "help")
		{
			return {
				"scene.get",		 "scene.new",		"scene.replace",	   "scene.load",	   "scene.save",
				"entity.create",	 "entity.destroy",	"entity.parent",	   "entity.patch",	   "prefab.spawn",
				"history.undo",		 "history.redo",	"history.begin",	   "history.commit",   "history.cancel",
				"simulation.start",	 "simulation.step", "simulation.stop",	   "input.set",		   "project.export",
				"scene.environment", "scene.status",	"project.get",		   "project.open",	   "project.create",
				"session.close",	 "recovery.list",	"recovery.checkpoint", "recovery.restore", "recovery.discard",
				"project.configure", "project.browse",	"project.recent",	   "project.forget"};
		}
		if (command == "recovery.list")
		{
			EnableRecovery();
			return m_Recovery->List();
		}
		if (command == "recovery.checkpoint")
		{
			CheckpointRecovery();
			return {{"checkpointed", HasUnsavedChanges()}};
		}
		if (command == "recovery.restore")
		{
			return RestoreRecovery(request);
		}
		if (command == "recovery.discard")
		{
			EnableRecovery();
			m_Recovery->Discard(request.at("session").get<std::string>());
			return {{"discarded", true}};
		}
		if (command == "session.close")
		{
			RequireDocumentChange(request);
			if (m_Recovery)
			{
				m_Recovery->Clear();
			}
			m_IsClosed = true;
			return {{"closed", true}};
		}
		if (command == "scene.status")
		{
			return {{"path", m_ScenePath ? nlohmann::json(*m_ScenePath) : nlohmann::json(nullptr)},
					{"dirty", HasUnsavedChanges()},
					{"editing", m_EditTransaction.has_value()},
					{"playing", IsPlaying()}};
		}
		if (command == "project.browse")
		{
			return BrowseProjects(request.at("path").get<std::string>());
		}
		if (command == "project.recent")
		{
			auto paths = nlohmann::json::array();
			if (m_ProjectHistory)
			{
				for (const auto& path : m_ProjectHistory->List())
				{
					paths.push_back(path.generic_string());
				}
			}
			return {{"enabled", m_ProjectHistory != nullptr}, {"projects", paths}};
		}
		if (command == "project.forget")
		{
			if (!m_ProjectHistory)
			{
				throw std::logic_error("Enable editor history before removing recent projects");
			}
			m_ProjectHistory->Forget(request.at("path").get<std::string>());
			return {{"forgotten", true}};
		}
		if (command == "project.get")
		{
			return {{"path",
					 m_Project ? nlohmann::json(m_Project->GetFilePath().generic_string()) : nlohmann::json(nullptr)},
					{"assets", m_ProjectRoot.generic_string()},
					{"config", m_Project ? m_Project->Serialize() : nlohmann::json(nullptr)},
					{"startupError", m_StartupError ? nlohmann::json(*m_StartupError) : nlohmann::json(nullptr)}};
		}
		if (command == "project.open" || command == "project.create")
		{
			RequireDocumentChange(request);
			const auto path = request.at("path").get<std::string>();
			const bool repair = request.value("repairStartup", false);
			if (repair && command != "project.open")
			{
				throw std::invalid_argument("Startup repair applies only to opening an existing project");
			}
			OpenProject(command == "project.open" ? (repair ? Project::LoadForRepair(path) : Project::Load(path))
												  : Project::Create(path, request.at("name").get<std::string>()),
						repair);
			auto warning = m_StartupError;
			if (const auto historyError = RememberProject())
			{
				warning = warning ? *warning + "\n" + *historyError : *historyError;
			}
			return {{"path", m_Project->GetFilePath().generic_string()},
					{"assets", m_ProjectRoot.generic_string()},
					{"warning", warning ? nlohmann::json(*warning) : nlohmann::json(nullptr)}};
		}
		if (command == "project.configure")
		{
			return ConfigureProject(request);
		}
		if (command == "project.export")
		{
			RequireEditing();
			if (m_EditTransaction || HasUnsavedChanges())
			{
				throw std::logic_error("Finish editing and save the scene before exporting");
			}
			if (m_StartupError)
			{
				throw std::logic_error("Repair the project startup scene before exporting");
			}
			ExportSettings settings;
			m_DocumentFile.VerifyUnchanged();
			if (m_Project)
			{
				m_Project->VerifyUnchanged();
			}
			settings.AssetRoot = m_ProjectRoot;
			settings.ScenePath = request.at("scene").get<std::string>();
			settings.RuntimeExecutable = request.at("runtime").get<std::string>();
			settings.ThirdPartyNotices = request.at("notices").get<std::string>();
			settings.OutputDirectory = request.at("output").get<std::string>();
			return {{"path", ProjectExporter::Export(settings).generic_string()}};
		}
		if (command == "scene.get")
		{
			return m_Scene.Serialize();
		}
		if (command == "input.set")
		{
			if (!IsPlaying())
			{
				throw std::logic_error("Start simulation before sending input");
			}
			const auto& values = request.at("input");
			if (!values.is_object())
			{
				throw std::invalid_argument("Input must be an object");
			}
			InputSnapshot input;
			for (const auto& [key, value] : values.items())
			{
				if (key == "KeysDown" || key == "KeysPressed" || key == "KeysReleased")
				{
					auto& keys = key == "KeysDown"		? input.KeysDown
								 : key == "KeysPressed" ? input.KeysPressed
														: input.KeysReleased;
					if (!value.is_array())
					{
						throw std::invalid_argument("Keys must be a string array");
					}
					for (const auto& name : value)
					{
						keys.insert(name.get<std::string>());
					}
				}
				else if (key == "MouseDown" || key == "MousePressed" || key == "MouseReleased")
				{
					auto& buttons = key == "MouseDown"		? input.MouseDown
									: key == "MousePressed" ? input.MousePressed
															: input.MouseReleased;
					if (!value.is_array() || value.size() != buttons.size())
					{
						throw std::invalid_argument("Mouse buttons require eight booleans");
					}
					for (size_t index = 0; index < buttons.size(); ++index)
					{
						buttons[index] = value.at(index).get<bool>();
					}
				}
				else if (key == "MousePosition" || key == "MouseDelta" || key == "Wheel")
				{
					if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
					{
						throw std::invalid_argument("Mouse vectors require two numbers");
					}
					auto& vector = key == "MousePosition" ? input.MousePosition
								   : key == "MouseDelta"  ? input.MouseDelta
														  : input.Wheel;
					vector = {value[0].get<float>(), value[1].get<float>()};
				}
				else if (key == "Focused")
				{
					input.Focused = value.get<bool>();
				}
				else
				{
					throw std::invalid_argument("Unknown input field: " + key);
				}
			}
			InputState::Validate(input);
			m_Simulation->SetInput(input);
			return {{"accepted", true}};
		}
		if (command == "scene.save")
		{
			RequireEditing();
			if (m_EditTransaction)
			{
				throw std::logic_error("Finish the current edit before saving");
			}
			auto path = request.at("path").get<std::string>();
			auto saved = m_Scene.Serialize();
			m_DocumentFile.Save(m_Scene, ResolvePath(path));
			m_ScenePath = std::move(path);
			m_SavedScene = std::move(saved);
			m_RecoveryPending = true;
			return {{"saved", true}};
		}
		if (command == "simulation.start")
		{
			RequireEditing();
			if (m_StartupError)
			{
				throw std::logic_error("Repair the project startup scene before starting simulation");
			}
			if (m_EditTransaction)
			{
				throw std::logic_error("Finish the current edit before starting simulation");
			}
			m_PlaySnapshot = m_Scene.Serialize();
			auto settings = m_SimulationSettings;
			if (request.contains("audio"))
			{
				const auto audio = request.at("audio").get<std::string>();
				if (audio == "device")
				{
					settings.Audio = AudioMode::Device;
				}
				else if (audio == "offline")
				{
					settings.Audio = AudioMode::Offline;
				}
				else if (audio == "disabled")
				{
					settings.Audio = AudioMode::Disabled;
				}
				else
				{
					throw std::invalid_argument("Audio must be device, offline, or disabled");
				}
			}
			auto simulation = std::make_unique<Simulation>(m_Scene, m_ProjectRoot, settings);
			try
			{
				simulation->Start();
			}
			catch (...)
			{
				simulation.reset();
				m_Scene.ReplaceFromJson(m_PlaySnapshot);
				throw;
			}
			m_Simulation = std::move(simulation);
			return {{"playing", true}, {"errors", m_Simulation->GetErrors()}};
		}
		if (command == "simulation.step")
		{
			if (!IsPlaying())
			{
				throw std::logic_error("Simulation is not running");
			}
			const int steps = static_cast<int>(ReadUnsigned(request.value("steps", nlohmann::json(1)), 10000));
			if (steps < 1 || steps > 10000)
			{
				throw std::invalid_argument("Steps must be in [1, 10000]");
			}
			for (int step = 0; step < steps; ++step)
			{
				m_Simulation->Step();
			}
			return {
				{"scene", m_Scene.Serialize()}, {"errors", m_Simulation->GetErrors()}, {"log", m_Simulation->GetLog()}};
		}
		if (command == "simulation.stop")
		{
			if (!IsPlaying())
			{
				throw std::logic_error("Simulation is not running");
			}
			m_Simulation->Stop();
			const auto errors = m_Simulation->GetErrors();
			m_Simulation.reset();
			m_Scene.ReplaceFromJson(m_PlaySnapshot);
			m_RecoveryPending = true;
			return {{"playing", false}, {"errors", errors}};
		}
		RequireEditing();
		if (command == "scene.new" || command == "scene.load")
		{
			RequireDocumentChange(request);
			std::optional<std::string> scenePath;
			nlohmann::json saved;
			SceneDocumentFile file;
			auto scene = command == "scene.new" ? Scene(request.value("name", "Untitled"))
												: file.Load(ResolvePath(request.at("path").get<std::string>()));
			if (command == "scene.load")
			{
				scenePath = request.at("path").get<std::string>();
				saved = scene.Serialize();
			}
			if (m_Recovery)
			{
				m_Recovery->Clear();
			}
			m_Scene = std::move(scene);
			m_DocumentFile = std::move(file);
			m_ScenePath = std::move(scenePath);
			m_SavedScene = std::move(saved);
			// History belongs to a document; it cannot resurrect another scene.
			m_Undo.clear();
			m_Redo.clear();
			m_RecoveryPending = true;
			return nlohmann::json::object();
		}
		if (command == "history.begin")
		{
			if (m_EditTransaction)
			{
				throw std::logic_error("An edit transaction is already active");
			}
			m_EditTransaction = m_Scene.Serialize();
			return {{"editing", true}};
		}
		if (command == "history.commit" || command == "history.cancel")
		{
			if (!m_EditTransaction)
			{
				throw std::logic_error("There is no active edit transaction");
			}
			auto before = std::move(*m_EditTransaction);
			m_EditTransaction.reset();
			if (command == "history.cancel")
			{
				if (m_Scene.Serialize() != before)
				{
					m_Scene.ReplaceFromJson(before);
					m_RecoveryPending = true;
				}
			}
			else
			{
				RecordChange(before);
			}
			return {{"editing", false}};
		}
		if (command == "history.undo" || command == "history.redo")
		{
			if (m_EditTransaction)
			{
				throw std::logic_error("Finish the current edit before traversing history");
			}
			auto& source = command == "history.undo" ? m_Undo : m_Redo;
			auto& destination = command == "history.undo" ? m_Redo : m_Undo;
			if (source.empty())
			{
				throw std::logic_error("History is empty");
			}
			destination.push_back(m_Scene.Serialize());
			m_Scene.ReplaceFromJson(source.back());
			source.pop_back();
			m_RecoveryPending = true;
			return m_Scene.Serialize();
		}

		const auto before = m_Scene.Serialize();
		nlohmann::json result = nlohmann::json::object();
		try
		{
			if (command == "scene.replace")
			{
				m_Scene.ReplaceFromJson(request.at("scene"));
			}
			else if (command == "scene.environment")
			{
				const auto& environment = request.at("environment");
				if (!environment.is_object())
				{
					throw std::invalid_argument("Environment changes must be an object");
				}
				auto document = before;
				document["Environment"].merge_patch(environment);
				m_Scene.ReplaceFromJson(document);
			}
			else if (command == "entity.create")
			{
				const Entity entity = m_Scene.CreateEntity(request.value("name", "Entity"));
				result["entity"] = m_Scene.GetPersistentID(entity);
			}
			else if (command == "entity.destroy")
			{
				m_Scene.DestroyEntity(RequireEntity(ReadEntityID(request, "entity")));
			}
			else if (command == "entity.parent")
			{
				const auto parentID = request.contains("parent") ? ReadEntityID(request, "parent") : 0;
				m_Scene.SetParent(RequireEntity(ReadEntityID(request, "entity")),
								  parentID ? RequireEntity(parentID) : Entity{});
			}
			else if (command == "entity.patch")
			{
				const auto entityID = ReadEntityID(request, "entity");
				(void)RequireEntity(entityID);
				auto document = before;
				const auto& patch = request.at("patch");
				if (!patch.is_object() || patch.contains("ID") || patch.contains("Parent"))
				{
					throw std::invalid_argument("Patch must be an object; use entity.parent for hierarchy changes");
				}
				for (auto& record : document.at("Entities"))
				{
					if (record.at("ID").get<uint64_t>() == entityID)
					{
						record.merge_patch(patch);
					}
				}
				m_Scene.ReplaceFromJson(document);
			}
			else if (command == "prefab.spawn")
			{
				auto prefab = Scene::Load(ResolvePath(request.at("path").get<std::string>()));
				const auto source = prefab.FindByID(ReadEntityID(request, "root"));
				const auto parentID = request.contains("parent") ? ReadEntityID(request, "parent") : 0;
				const auto entity =
					m_Scene.InstantiatePrefab(prefab, source, parentID ? RequireEntity(parentID) : Entity{});
				result["entity"] = m_Scene.GetPersistentID(entity);
			}
			else
			{
				throw std::invalid_argument("Unknown command: " + command);
			}
			m_Scene.Validate();
			RecordChange(before);
		}
		catch (...)
		{
			// Preserve existing handles when the rejected edit did not mutate anything.
			if (m_Scene.Serialize() != before)
			{
				m_Scene.ReplaceFromJson(before);
			}
			throw;
		}
		return result;
	}
} // namespace Aster
