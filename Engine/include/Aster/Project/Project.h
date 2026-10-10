#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>

namespace Aster
{
	struct ProjectConfig
	{
		std::string Name = "Untitled";
		std::filesystem::path AssetDirectory = "Assets";
		std::filesystem::path StartScene = "Scenes/Main.aster";
	};

	// A project owns its configuration and paths; there is no global active project.
	// Serialized paths are portable and relative to the project or its asset root.
	class Project
	{
	  public:
		[[nodiscard]] static Project Load(const std::filesystem::path& filePath);
		// Editor repair only: validate configuration/containment without reading the
		// startup scene. Updating configuration still requires a valid startup scene.
		[[nodiscard]] static Project LoadForRepair(const std::filesystem::path& filePath);
		// Destination must be absent, with an existing parent. Publishes a complete
		// project containing a camera scene through a sibling staging directory.
		[[nodiscard]] static Project Create(const std::filesystem::path& directory, std::string name);
		[[nodiscard]] static ProjectConfig DeserializeConfig(const nlohmann::json& document);
		[[nodiscard]] nlohmann::json Serialize() const;
		// Validate a candidate without writing or changing this instance. The copy
		// retains the exact loaded file baseline for a later UpdateConfig.
		[[nodiscard]] Project PreviewConfig(ProjectConfig config) const;
		// Validates configuration and the startup scene, then saves before changing
		// this instance. Native writer ownership and exact loaded bytes protect against
		// cooperating writers/external edits. Rejected updates preserve configuration.
		void UpdateConfig(ProjectConfig config);
		void VerifyUnchanged() const;
		[[nodiscard]] const ProjectConfig& GetConfig() const noexcept;
		[[nodiscard]] const std::filesystem::path& GetFilePath() const noexcept;
		[[nodiscard]] const std::filesystem::path& GetAssetDirectory() const noexcept;
		[[nodiscard]] std::filesystem::path ResolveAssetPath(const std::filesystem::path& relative) const;

	  private:
		Project(std::filesystem::path filePath, ProjectConfig config, std::string contents = {},
				bool requireStartup = true);
		static Project ReadConfiguration(const std::filesystem::path& filePath, bool requireStartup);
		std::filesystem::path ValidateConfig(const ProjectConfig& config, bool requireStartup = true) const;

		std::filesystem::path m_FilePath;
		std::filesystem::path m_AssetDirectory;
		ProjectConfig m_Config;
		std::string m_Contents;
	};
} // namespace Aster
