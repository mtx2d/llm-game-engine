#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

void RunRendererTests(const std::filesystem::path& evidence);

int main(int argc, char** argv)
{
	try
	{
		std::filesystem::path evidence;
		if (argc != 1)
		{
			if (argc != 3 || std::string_view(argv[1]) != "--evidence" || std::string_view(argv[2]).empty())
			{
				throw std::invalid_argument("Usage: AsterRendererTests [--evidence <directory>]");
			}
			evidence = argv[2];
			std::filesystem::create_directories(evidence);
			// A failed rerun must not leave the previous run's successful index.
			std::filesystem::remove(evidence / "Index.json");
		}
		RunRendererTests(evidence);
		std::cout << "Renderer tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Renderer test failure: " << error.what() << '\n';
		return 1;
	}
}
