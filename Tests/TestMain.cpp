#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

void RunSceneTests();
void RunSimulationTests();
void RunCommandTests();
void RunAssetTests();
void RunEnvironmentTests();
void RunViewportTests();

int main(int argc, char** argv)
{
	try
	{
		const std::string_view suite = argc > 1 ? argv[1] : "all";
		if (suite == "scene" || suite == "all")
		{
			RunSceneTests();
			std::cout << "Scene tests passed\n";
		}
		if (suite == "simulation" || suite == "all")
		{
			RunSimulationTests();
			std::cout << "Simulation tests passed\n";
		}
		if (suite == "commands" || suite == "all")
		{
			RunCommandTests();
			std::cout << "Command tests passed\n";
		}
		if (suite == "assets" || suite == "all")
		{
			RunAssetTests();
			std::cout << "Asset tests passed\n";
		}
		if (suite == "environment" || suite == "all")
		{
			RunEnvironmentTests();
			std::cout << "Environment tests passed\n";
		}
		if (suite == "viewport" || suite == "all")
		{
			RunViewportTests();
			std::cout << "Viewport tests passed\n";
		}
		if (suite != "scene" && suite != "simulation" && suite != "commands" && suite != "assets" &&
			suite != "environment" && suite != "viewport" && suite != "all")
		{
			throw std::invalid_argument("Unknown test suite");
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Test failure: " << error.what() << '\n';
		return 1;
	}
}
