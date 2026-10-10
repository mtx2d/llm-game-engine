#include <Aster/Core/JsonFile.h>
#include <Aster/Project/Project.h>
#include <Aster/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <stdexcept>
#include <utility>

namespace Aster
{
	namespace
	{
		std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& relative)
		{
			Scene::ValidateAssetPath(relative.generic_string());
			if (relative.empty() || relative.generic_string().find('\\') != std::string::npos)
			{
				throw std::invalid_argument("Project paths must use forward slashes and must not be empty");
			}
			const auto resolved = std::filesystem::weakly_canonical(root / relative);
			auto candidate = resolved.begin();
			for (const auto& part : root)
			{
				if (candidate == resolved.end() || *candidate++ != part)
				{
					throw std::invalid_argument("Project path escapes its root: " + relative.generic_string());
				}
			}
			return resolved;
		}

		nlohmann::json SerializeConfig(const ProjectConfig& config)
		{
			return {{"Version", 1},
					{"Name", config.Name},
					{"AssetDirectory", config.AssetDirectory.generic_string()},
					{"StartScene", config.StartScene.generic_string()}};
		}
	} // namespace

	Project::Project(std::filesystem::path filePath, ProjectConfig config)
		: m_FilePath(std::move(filePath)), m_Config(std::move(config))
	{
		m_AssetDirectory = ValidateConfig(m_Config);
	}

	std::filesystem::path Project::ValidateConfig(const ProjectConfig& config) const
	{
		if (config.Name.empty() || config.Name.size() > 128 ||
			std::any_of(config.Name.begin(), config.Name.end(),
						[](unsigned char value) { return value < 32 || value == 127; }))
		{
			throw std::invalid_argument("Project name must contain 1 to 128 bytes without control characters");
		}
		// dump also validates UTF-8 in names and serialized filesystem paths.
		(void)SerializeConfig(config).dump();
		const auto assetDirectory = Resolve(m_FilePath.parent_path(), config.AssetDirectory);
		if (!std::filesystem::is_directory(assetDirectory))
		{
			throw std::invalid_argument("Project asset directory does not exist");
		}
		const auto startScene = Resolve(assetDirectory, config.StartScene);
		if (config.StartScene.extension() != ".aster")
		{
			throw std::invalid_argument("Project startup scene must have the .aster extension");
		}
		(void)Scene::Load(startScene);
		return assetDirectory;
	}

	ProjectConfig Project::DeserializeConfig(const nlohmann::json& document)
	{
		if (!document.is_object() || document.size() != 4 || !document.contains("Version") ||
			!document.at("Version").is_number_integer() || document.at("Version") != 1)
		{
			throw std::invalid_argument(
				"Unsupported project format; expected Version, Name, AssetDirectory and StartScene");
		}
		const auto assets = document.at("AssetDirectory").get<std::string>();
		const auto scene = document.at("StartScene").get<std::string>();
		if (assets.find('\\') != std::string::npos || scene.find('\\') != std::string::npos)
		{
			throw std::invalid_argument("Serialized project paths must use forward slashes");
		}
		return {document.at("Name").get<std::string>(), assets, scene};
	}

	Project Project::Load(const std::filesystem::path& filePath)
	{
		if (filePath.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
			filePath.extension() != ".asterproj" || std::filesystem::is_symlink(filePath))
		{
			throw std::invalid_argument("Open a .asterproj file that is not a symbolic link");
		}
		const auto canonical = std::filesystem::canonical(filePath);
		return Project(canonical, DeserializeConfig(ReadJsonFile(canonical, 64 * 1024, 8)));
	}

	Project Project::Create(const std::filesystem::path& directory, std::string name)
	{
		if (directory.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
			directory.filename().empty() || directory.filename() == "." || directory.filename() == "..")
		{
			throw std::invalid_argument("New project must name an absent directory");
		}
		const auto parent = std::filesystem::canonical(directory.has_parent_path() ? directory.parent_path() : ".");
		const auto destination = parent / directory.filename();
		if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
		{
			throw std::invalid_argument("New project destination already exists");
		}
		std::random_device random;
		std::filesystem::path staging;
		bool created = false;
		for (int attempt = 0; attempt < 16 && !created; ++attempt)
		{
			staging = parent / (".aster-project-" + std::to_string(random()) + "-" + std::to_string(random()));
			created = std::filesystem::create_directory(staging);
		}
		if (!created)
		{
			throw std::runtime_error("Cannot allocate project staging directory");
		}
		try
		{
			std::filesystem::create_directories(staging / "Assets/Scenes");
			Scene scene("Main");
			const auto camera = scene.CreateEntity("Camera");
			scene.Get(camera).Camera = CameraComponent{};
			scene.Get(camera).Transform.Translation = {0, 2, 5};
			scene.Save(staging / "Assets/Scenes/Main.aster");
			Project project(staging / "Project.asterproj", {std::move(name), "Assets", "Scenes/Main.aster"});
			WriteTextFileAtomically(project.GetFilePath(), project.Serialize().dump(2));
			if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
			{
				throw std::invalid_argument("New project destination appeared during creation");
			}
			project.m_FilePath = destination / "Project.asterproj";
			project.m_AssetDirectory = destination / "Assets";
			std::filesystem::rename(staging, destination);
			return project;
		}
		catch (...)
		{
			std::error_code cleanupError;
			std::filesystem::remove_all(staging, cleanupError);
			throw;
		}
	}

	nlohmann::json Project::Serialize() const
	{
		return SerializeConfig(m_Config);
	}

	void Project::UpdateConfig(ProjectConfig config)
	{
		auto assetDirectory = ValidateConfig(config);
		WriteTextFileAtomically(m_FilePath, SerializeConfig(config).dump(2));
		m_Config = std::move(config);
		m_AssetDirectory = std::move(assetDirectory);
	}

	const ProjectConfig& Project::GetConfig() const noexcept
	{
		return m_Config;
	}

	const std::filesystem::path& Project::GetFilePath() const noexcept
	{
		return m_FilePath;
	}

	const std::filesystem::path& Project::GetAssetDirectory() const noexcept
	{
		return m_AssetDirectory;
	}

	std::filesystem::path Project::ResolveAssetPath(const std::filesystem::path& relative) const
	{
		return Resolve(m_AssetDirectory, relative);
	}
} // namespace Aster
