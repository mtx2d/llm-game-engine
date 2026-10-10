#include <Aster/Editor/CommandProcessor.h>

#ifdef ASTER_HAS_RENDERER
#include <Aster/Assets/AssetImporter.h>
#include <Aster/Editor/GuiLayer.h>
#include <Aster/Renderer/Renderer.h>
#endif

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
	void StopSimulation(Aster::CommandProcessor& processor)
	{
		if (!processor.IsPlaying())
		{
			return;
		}
		const auto response = processor.Execute({{"command", "simulation.stop"}});
		if (!response.at("ok").get<bool>())
		{
			throw std::runtime_error(response.at("error").get<std::string>());
		}
		const auto& errors = response.at("result").at("errors");
		if (!errors.empty())
		{
			throw std::runtime_error(errors.front().get<std::string>());
		}
	}

	int RunAutomation(const std::filesystem::path& project,
					  const std::optional<std::filesystem::path>& editorState = std::nullopt)
	{
		Aster::CommandProcessor processor(project);
		if (editorState)
		{
			processor.EnableProjectHistory(*editorState);
		}
		processor.EnableRecovery();
		std::string line;
		while (!processor.IsClosed() && std::getline(std::cin, line))
		{
			try
			{
				auto response = processor.Execute(nlohmann::json::parse(line));
				if (const auto warning = processor.UpdateRecovery(true))
				{
					response["warnings"] = {*warning};
				}
				std::cout << response.dump() << '\n' << std::flush;
			}
			catch (const std::exception& error)
			{
				std::cout << nlohmann::json({{"ok", false}, {"error", error.what()}}).dump() << '\n' << std::flush;
			}
		}
		StopSimulation(processor);
		return std::cin.bad() ? 1 : 0;
	}

#ifdef ASTER_HAS_RENDERER
	void SaveScreenshot(const Aster::RenderImage& image, const std::filesystem::path& path)
	{
		std::ofstream output(path, std::ios::binary);
		output.exceptions(std::ios::badbit | std::ios::failbit);
		output << "P6\n" << image.Width << ' ' << image.Height << "\n255\n";
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
		{
			output.write(reinterpret_cast<const char*>(image.Pixels.data() + offset), 3);
		}
	}
#endif
} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc == 3 && std::string(argv[1]) == "--automation")
		{
			return RunAutomation(argv[2]);
		}
		if (argc == 5 && std::string(argv[1]) == "--automation" && std::string(argv[3]) == "--editor-state")
		{
			return RunAutomation(argv[2], std::filesystem::path(argv[4]));
		}
		if (argc == 4 && std::string(argv[1]) == "--create-project")
		{
			const auto project = Aster::Project::Create(argv[2], argv[3]);
			std::cout << nlohmann::json({{"project", project.GetFilePath().generic_string()}}).dump() << '\n';
			return 0;
		}
		if (argc == 2 && std::string(argv[1]) == "--help")
		{
			std::cout << "AsterEditor --automation <asset-directory|project.asterproj> [--editor-state <directory>]\n"
					  << "AsterEditor --create-project <new-directory> <name>\n"
					  << "AsterEditor [--project <asset-directory|project.asterproj>] [--scene <relative-scene>] "
						 "[--frames N] "
						 "[--screenshot <image.ppm>] [--audio device|offline|disabled] [--play] "
						 "[--launcher] [--editor-state <directory>]\n";
			return 0;
		}
#ifdef ASTER_HAS_RENDERER
		auto project = std::filesystem::current_path();
		if (std::filesystem::is_directory(project / "Assets"))
		{
			project /= "Assets";
		}
		std::optional<std::filesystem::path> scenePath;
		std::filesystem::path screenshot;
		std::filesystem::path editorState = std::filesystem::current_path();
		bool explicitEditorState = false;
		bool explicitProject = false;
		bool showLauncher = false;
		int maximumFrames = 0;
		bool startPlaying = false;
		Aster::SimulationSettings simulationSettings;
		simulationSettings.Audio = Aster::AudioMode::Device;
		for (int index = 1; index < argc; ++index)
		{
			const std::string argument = argv[index];
			if (argument == "--launcher")
			{
				showLauncher = true;
				continue;
			}
			if (argument == "--play")
			{
				startPlaying = true;
				continue;
			}
			if (index + 1 >= argc)
			{
				throw std::invalid_argument("Missing value for " + argument);
			}
			const std::string value = argv[++index];
			if (argument == "--project")
			{
				project = value;
				explicitProject = true;
			}
			else if (argument == "--editor-state")
			{
				editorState = value;
				explicitEditorState = true;
			}
			else if (argument == "--scene")
			{
				scenePath = value;
			}
			else if (argument == "--screenshot")
			{
				screenshot = value;
			}
			else if (argument == "--audio")
			{
				if (value == "device")
				{
					simulationSettings.Audio = Aster::AudioMode::Device;
				}
				else if (value == "offline")
				{
					simulationSettings.Audio = Aster::AudioMode::Offline;
				}
				else if (value == "disabled")
				{
					simulationSettings.Audio = Aster::AudioMode::Disabled;
				}
				else
				{
					throw std::invalid_argument("Audio must be device, offline, or disabled");
				}
			}
			else if (argument == "--frames")
			{
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), maximumFrames);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || maximumFrames < 1 ||
					maximumFrames > 100000)
				{
					throw std::invalid_argument("Frames must be an integer in [1, 100000]");
				}
			}
			else
			{
				throw std::invalid_argument("Unknown argument: " + argument);
			}
		}
		if (showLauncher && startPlaying)
		{
			throw std::invalid_argument("Choose a project in the launcher before starting play");
		}
		Aster::CommandProcessor processor(project, simulationSettings, maximumFrames == 0 && !startPlaying);
		std::optional<std::string> historyError;
		if (maximumFrames == 0 || explicitEditorState)
		{
			try
			{
				processor.EnableProjectHistory(editorState);
			}
			catch (const std::exception& error)
			{
				historyError = "Recent-project history unavailable: " + std::string(error.what());
			}
		}
		showLauncher = showLauncher || (!explicitProject && !scenePath && maximumFrames == 0 && !startPlaying);
		project = processor.GetAssetRoot();
		if (!scenePath && processor.GetScenePath())
		{
			scenePath = *processor.GetScenePath();
		}
		if (!scenePath && !showLauncher && !processor.GetStartupError() &&
			std::filesystem::is_regular_file(project / "Scenes/FeatureGallery.aster"))
		{
			scenePath = "Scenes/FeatureGallery.aster";
		}
		Aster::AssetImporter importer(project);
		Aster::RendererOptions options;
		options.Headless = false;
		options.Width = 1440;
		options.Height = 900;
		options.Title = "Aster — 3D Game Editor";
		Aster::Renderer renderer(options);
		{
			Aster::GuiLayer gui(renderer, processor, project, scenePath);
			if (showLauncher && !processor.GetStartupError())
			{
				gui.ShowProjectLauncher();
			}
			if (historyError)
			{
				gui.SetStatus(processor.GetStartupError() ? *processor.GetStartupError() + "\n" + *historyError
														  : *historyError);
			}
			processor.EnableRecovery();
			if (startPlaying)
			{
				const auto response = processor.Execute({{"command", "simulation.start"}});
				if (!response.at("ok").get<bool>())
				{
					throw std::runtime_error(response.at("error").get<std::string>());
				}
				if (!processor.GetSimulationErrors().empty())
				{
					throw std::runtime_error(processor.GetSimulationErrors().front());
				}
			}
			int frame = 0;
			while (!processor.IsClosed() && (maximumFrames == 0 || frame < maximumFrames))
			{
				renderer.PollEvents();
				gui.RunFrame();
				if (processor.IsClosed())
				{
					break;
				}
				try
				{
					if (importer.GetRoot() != processor.GetAssetRoot())
					{
						importer = Aster::AssetImporter(processor.GetAssetRoot());
					}
					renderer.RenderScene(processor.GetScene(), importer, gui.GetRenderSettings());
				}
				catch (const std::exception& error)
				{
					if (maximumFrames != 0)
					{
						throw;
					}
					gui.SetStatus(error.what());
					renderer.RenderFrame();
				}
				++frame;
			}
			if (!screenshot.empty())
			{
				SaveScreenshot(renderer.ReadbackRgba8(), screenshot);
			}
			// Teardown callbacks run before deciding whether shutdown succeeded.
			StopSimulation(processor);
		}
		renderer.Shutdown();
		if (!renderer.GetValidationMessages().empty())
		{
			throw std::runtime_error(renderer.GetValidationMessages().front());
		}
		return 0;
#else
		throw std::invalid_argument(
			"This editor supports --automation only; configure ASTER_BUILD_RENDERER=ON for the graphical editor");
#endif
	}
	catch (const std::exception& error)
	{
		std::cerr << "Aster editor: " << error.what() << '\n';
		return 1;
	}
}
