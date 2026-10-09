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

	int RunAutomation(const std::filesystem::path& project)
	{
		Aster::CommandProcessor processor(project);
		std::string line;
		while (std::getline(std::cin, line))
		{
			try
			{
				std::cout << processor.Execute(nlohmann::json::parse(line)).dump() << '\n' << std::flush;
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
		if (argc == 2 && std::string(argv[1]) == "--help")
		{
			std::cout << "AsterEditor --automation <project-directory>\n"
					  << "AsterEditor [--project <asset-directory>] [--scene <relative-scene>] [--frames N] "
						 "[--screenshot <image.ppm>] [--audio device|offline|disabled] [--play]\n";
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
		int maximumFrames = 0;
		bool startPlaying = false;
		Aster::SimulationSettings simulationSettings;
		simulationSettings.Audio = Aster::AudioMode::Device;
		for (int index = 1; index < argc; ++index)
		{
			const std::string argument = argv[index];
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
		if (!scenePath && std::filesystem::is_regular_file(project / "Scenes/FeatureGallery.aster"))
		{
			scenePath = "Scenes/FeatureGallery.aster";
		}
		Aster::CommandProcessor processor(project, simulationSettings);
		Aster::AssetImporter importer(project);
		Aster::RendererOptions options;
		options.Headless = false;
		options.Width = 1440;
		options.Height = 900;
		options.Title = "Aster — 3D Game Editor";
		Aster::Renderer renderer(options);
		{
			Aster::GuiLayer gui(renderer, processor, project, scenePath);
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
			while (!renderer.ShouldClose() && (maximumFrames == 0 || frame < maximumFrames))
			{
				renderer.PollEvents();
				gui.RunFrame();
				try
				{
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
