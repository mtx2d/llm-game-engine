#include <exception>
#include <iostream>

void RunRendererTests();

int main()
{
	try
	{
		RunRendererTests();
		std::cout << "Renderer tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Renderer test failure: " << error.what() << '\n';
		return 1;
	}
}
