#include <Aster/Editor/CommandProcessor.h>
#include <Aster/Editor/ProjectExporter.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace Aster
{
	namespace
	{
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

	CommandProcessor::CommandProcessor(std::filesystem::path projectRoot, SimulationSettings simulationSettings)
		: m_ProjectRoot(std::filesystem::weakly_canonical(std::move(projectRoot))),
		  m_SimulationSettings(simulationSettings)
	{
		if (!std::filesystem::is_directory(m_ProjectRoot))
		{
			throw std::invalid_argument("Project root must be an existing directory");
		}
	}

	void CommandProcessor::UpdateSimulation(double deltaTime)
	{
		if (m_Simulation)
		{
			m_Simulation->Update(deltaTime);
		}
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
			if (!request.is_object())
			{
				throw std::invalid_argument("Request must be a JSON object");
			}
			response["result"] = Dispatch(request);
			response["ok"] = true;
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
			return {"scene.get",		"scene.new",	   "scene.replace",	  "scene.load",		"scene.save",
					"entity.create",	"entity.destroy",  "entity.parent",	  "entity.patch",	"prefab.spawn",
					"history.undo",		"history.redo",	   "history.begin",	  "history.commit", "history.cancel",
					"simulation.start", "simulation.step", "simulation.stop", "input.set",		"project.export",
					"scene.environment"};
		}
		if (command == "project.export")
		{
			RequireEditing();
			ExportSettings settings;
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
			m_Scene.Save(ResolvePath(request.at("path").get<std::string>()));
			return {{"saved", true}};
		}
		if (command == "simulation.start")
		{
			RequireEditing();
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
			return {{"playing", false}, {"errors", errors}};
		}
		RequireEditing();
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
			return m_Scene.Serialize();
		}

		const auto before = m_Scene.Serialize();
		nlohmann::json result = nlohmann::json::object();
		try
		{
			if (command == "scene.new")
			{
				m_Scene = Scene(request.value("name", "Untitled"));
			}
			else if (command == "scene.replace")
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
			else if (command == "scene.load")
			{
				m_Scene = Scene::Load(ResolvePath(request.at("path").get<std::string>()));
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
