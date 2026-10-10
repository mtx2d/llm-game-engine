#pragma once

#include <Aster/Editor/RecoveryStore.h>
#include <Aster/Editor/SceneDocumentFile.h>
#include <Aster/Input/InputState.h>
#include <Aster/Project/Project.h>
#include <Aster/Scene/Scene.h>
#include <Aster/Simulation/Simulation.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace Aster
{
	// The editor and automation endpoint share this transactional authoring API.
	// Authoring is disabled during play; stopping restores the pre-play scene.
	class CommandProcessor
	{
	  public:
		explicit CommandProcessor(std::filesystem::path projectRoot, SimulationSettings simulationSettings = {},
								  bool allowStartupRepair = false);
		nlohmann::json Execute(const nlohmann::json& request);
		void UpdateSimulation(double deltaTime);
		void SetInput(const InputSnapshot& input);
		const std::vector<std::string>& GetSimulationErrors() const;
		const Scene& GetScene() const
		{
			return m_Scene;
		}
		bool IsPlaying() const
		{
			return m_Simulation != nullptr;
		}
		[[nodiscard]] const std::filesystem::path& GetAssetRoot() const noexcept
		{
			return m_ProjectRoot;
		}
		[[nodiscard]] const std::optional<std::string>& GetScenePath() const noexcept
		{
			return m_ScenePath;
		}
		[[nodiscard]] const std::optional<std::string>& GetStartupError() const noexcept
		{
			return m_StartupError;
		}
		[[nodiscard]] bool HasUnsavedChanges() const;
		void EnableRecovery();
		[[nodiscard]] std::optional<std::string> UpdateRecovery(bool force = false);
		[[nodiscard]] bool IsClosed() const noexcept
		{
			return m_IsClosed;
		}

	  private:
		nlohmann::json Dispatch(const nlohmann::json& request);
		std::filesystem::path ResolvePath(const std::string& relativePath) const;
		void RequireEditing() const;
		void RequireDocumentChange(const nlohmann::json& request) const;
		void OpenProject(Project project, bool allowStartupRepair = false);
		nlohmann::json ConfigureProject(const nlohmann::json& request);
		void RecordChange(const nlohmann::json& before);
		void CheckpointRecovery();
		nlohmann::json RestoreRecovery(const nlohmann::json& request);
		Entity RequireEntity(uint64_t id) const;

		std::filesystem::path m_ProjectRoot;
		SimulationSettings m_SimulationSettings;
		Scene m_Scene;
		SceneDocumentFile m_DocumentFile;
		bool m_IsClosed = false;
		std::optional<Project> m_Project;
		std::optional<std::string> m_StartupError;
		std::optional<std::string> m_ScenePath;
		nlohmann::json m_SavedScene;
		std::vector<nlohmann::json> m_Undo;
		std::vector<nlohmann::json> m_Redo;
		nlohmann::json m_PlaySnapshot;
		std::optional<nlohmann::json> m_EditTransaction;
		std::unique_ptr<Simulation> m_Simulation;
		std::unique_ptr<RecoveryStore> m_Recovery;
		bool m_RecoveryPending = false;
		std::chrono::steady_clock::time_point m_NextRecoveryCheckpoint{};
	};
} // namespace Aster
