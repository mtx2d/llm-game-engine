#include <Aster/Core/ExecutablePath.h>
#include <Aster/Scene/Scene.h>
#include <Aster/Simulation/Simulation.h>
#include <nlohmann/json.hpp>
#ifdef ASTER_HAS_RENDERER
#include <Aster/Assets/AssetImporter.h>
#include <Aster/Renderer/Renderer.h>
#endif

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
	try
	{
		std::filesystem::path scenePath;
		std::filesystem::path projectRoot = std::filesystem::current_path();
		std::filesystem::path outputPath;
		bool explicitProject = false;
		bool explicitSteps = false;
		bool headless = false;
		bool windowRequested = false;
		bool validationRequested = false;
		int steps = 60;
		for (int index = 1; index < argc; ++index)
		{
			const std::string argument = argv[index];
			if (argument == "--headless")
			{
				headless = true;
				continue;
			}
			if (argument == "--window")
			{
				windowRequested = true;
				continue;
			}
			if (argument == "--validation")
			{
				validationRequested = true;
				continue;
			}
			if (argument == "--help")
			{
				std::cout
					<< "AsterRuntime --scene <scene> [--project <asset-root>] [--steps N] [--output <state.json>]\n"
					<< "--steps runs deterministic headless simulation; add --window for a bounded graphical run.\n"
					<< "--validation enables Vulkan/NVRHI development checks for a graphical run.\n"
					<< "Without --steps, the graphical game runs until closed. Exported games read adjacent "
					   "Game.json.\n";
				return 0;
			}
			if (index + 1 == argc)
			{
				throw std::invalid_argument("Missing argument for " + argument);
			}
			const std::string value = argv[++index];
			if (argument == "--scene")
			{
				scenePath = value;
			}
			else if (argument == "--project")
			{
				projectRoot = value;
				explicitProject = true;
			}
			else if (argument == "--output")
			{
				outputPath = value;
			}
			else if (argument == "--steps")
			{
				explicitSteps = true;
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), steps);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || steps < 0 ||
					steps > 1000000)
				{
					throw std::invalid_argument("Steps must be an integer in [0, 1000000]");
				}
			}
			else
			{
				throw std::invalid_argument("Unknown argument: " + argument);
			}
		}
		if (scenePath.empty())
		{
			const auto executableDirectory = Aster::GetExecutablePath().parent_path();
			const auto manifestPath = executableDirectory / "Game.json";
			if (std::filesystem::exists(manifestPath) && std::filesystem::file_size(manifestPath) > 65536)
			{
				throw std::invalid_argument("Game manifest exceeds 64 KiB");
			}
			std::ifstream input(manifestPath);
			if (!input)
			{
				throw std::invalid_argument("Specify --scene or provide Game.json next to the executable; see --help");
			}
			const auto manifest = nlohmann::json::parse(input);
			for (const auto& [key, value] : manifest.items())
			{
				(void)value;
				if (key != "Version" && key != "Assets" && key != "Scene")
				{
					throw std::invalid_argument("Unknown game manifest field: " + key);
				}
			}
			if (!manifest.at("Version").is_number_integer() || manifest.at("Version") != 1)
			{
				throw std::invalid_argument("Unsupported game manifest version");
			}
			const auto assets = manifest.at("Assets").get<std::string>();
			const auto entryScene = manifest.at("Scene").get<std::string>();
			Aster::Scene::ValidateAssetPath(assets);
			Aster::Scene::ValidateAssetPath(entryScene);
			if (assets.empty() || entryScene.empty())
			{
				throw std::invalid_argument("Game manifest paths must not be empty");
			}
			if (!explicitProject)
			{
				projectRoot = executableDirectory / assets;
			}
			const auto contained = [](const std::filesystem::path& root, const std::filesystem::path& path)
			{
				auto iterator = path.begin();
				for (const auto& part : root)
				{
					if (iterator == path.end() || *iterator++ != part)
					{
						return false;
					}
				}
				return true;
			};
			projectRoot = std::filesystem::canonical(projectRoot);
			scenePath = std::filesystem::canonical(projectRoot / entryScene);
			if ((!explicitProject && !contained(executableDirectory, projectRoot)) ||
				!contained(projectRoot, scenePath))
			{
				throw std::invalid_argument("Game manifest path escapes its package");
			}
		}
		auto scene = Aster::Scene::Load(scenePath);
		headless = headless || (explicitSteps && !windowRequested);
#ifndef ASTER_HAS_RENDERER
		if (windowRequested)
		{
			throw std::invalid_argument("This runtime was built without graphical rendering");
		}
		headless = true;
#endif
		Aster::SimulationSettings simulationSettings;
		if (headless && validationRequested)
		{
			throw std::invalid_argument("--validation requires a graphical runtime run");
		}
		simulationSettings.Audio = headless || explicitSteps ? Aster::AudioMode::Offline : Aster::AudioMode::Device;
		Aster::Simulation simulation(scene, projectRoot, simulationSettings);
		simulation.Start();
		int renderedFrames = 0;
		if (headless)
		{
			for (int step = 0; step < steps; ++step)
			{
				simulation.Step();
			}
		}
#ifdef ASTER_HAS_RENDERER
		else
		{
			Aster::RendererOptions options;
			options.Headless = false;
			options.EnableValidation = validationRequested;
			options.Title = scene.GetName();
			Aster::Renderer renderer(options);
			Aster::AssetImporter importer(projectRoot);
			auto previous = std::chrono::steady_clock::now();
			while (!renderer.ShouldClose() && (!explicitSteps || renderedFrames < steps))
			{
				renderer.PollEvents();
				simulation.SetInput(renderer.GetInputSnapshot());
				const auto now = std::chrono::steady_clock::now();
				if (explicitSteps)
				{
					simulation.Step();
				}
				else
				{
					simulation.Update(std::chrono::duration<double>(now - previous).count());
				}
				previous = now;
				renderer.RenderScene(scene, importer);
				++renderedFrames;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			renderer.Shutdown();
			if (!renderer.GetValidationMessages().empty())
			{
				throw std::runtime_error("Renderer validation reported an error: " +
										 renderer.GetValidationMessages().front());
			}
		}
#endif
		simulation.Stop();
		const auto errors = simulation.GetErrors();
		if (!outputPath.empty())
		{
			scene.Save(outputPath);
		}
		std::cout << nlohmann::json({{"scene", scene.GetName()},
									 {"entities", scene.Size()},
									 {"steps", headless ? nlohmann::json(steps)
														: (explicitSteps ? nlohmann::json(renderedFrames)
																		 : nlohmann::json(nullptr))},
									 {"frames", renderedFrames},
									 {"errors", errors},
									 {"log", simulation.GetLog()}})
						 .dump()
				  << '\n';
		return errors.empty() ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Aster runtime: " << error.what() << '\n';
		return 1;
	}
}
