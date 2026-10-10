#include <Aster/Core/FileLock.h>
#include <Aster/Core/JsonFile.h>
#include <Aster/Editor/EditorStorage.h>
#include <Aster/Editor/SceneDocumentFile.h>
#include <Aster/Project/Project.h>

#include <iostream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const char* message)
	{
		bool rejected = false;
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Check(rejected, message);
	}

	void TestLocks(const std::filesystem::path& root)
	{
		const auto file = root / "Owner.lock";
		auto first = Aster::FileLock::TryAcquire(file);
		Check(first != nullptr, "First acquisition must own the lock");
		Check(!Aster::FileLock::TryAcquire(file), "Distinct handles in one process must contend");
		Check(!Aster::FileLock::TryAcquire(root / "./Owner.lock"), "Lexical aliases must contend");
		std::error_code error;
		std::filesystem::create_directory_symlink(root, root / "DirectoryAlias", error);
		if (!error)
		{
			Check(!Aster::FileLock::TryAcquire(root / "DirectoryAlias/Owner.lock"), "Physical parent aliases contend");
		}
#ifndef _WIN32
		Check(!error, "Parent alias test must run on Unix");
#endif
		Check(Aster::FileLock::TryAcquire(root / "Other.lock") != nullptr, "Distinct files are independent");
		first.reset();
		Check(std::filesystem::is_regular_file(file), "Release must retain the stable lock file");
		Check(Aster::FileLock::TryAcquire(file) != nullptr, "RAII release permits reacquisition");
		const auto contents = Aster::ReadTextFile(file, 1024);
		Check(contents.empty(), "Lock acquisition must not write/truncate file contents");
		Aster::WriteTextFileAtomically(file, "sentinel");
		{
			const auto lock = Aster::FileLock::TryAcquire(file);
			Check(lock != nullptr, "Existing populated file can be locked");
		}
		Check(Aster::ReadTextFile(file, 1024) == "sentinel", "Lock preserves existing bytes");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(root); }, "A directory cannot be locked as a file");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(root / "Missing/Owner.lock"); }, "Missing parent must fail");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(root / "."); }, "Dot path rejected");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(std::filesystem::path(std::string("bad\0path", 8))); },
				"Null path rejected before native open");
		std::filesystem::create_symlink(file, root / "Alias.lock", error);
		if (!error)
		{
			Rejects([&] { (void)Aster::FileLock::TryAcquire(root / "Alias.lock"); }, "Symlink lock rejected");
		}
#ifndef _WIN32
		Check(!error, "Symlink file test must run on Unix");
#endif
		std::filesystem::create_hard_link(file, root / "HardAlias.lock");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(file); }, "Multiply linked lock identity rejected");
		std::filesystem::remove(root / "HardAlias.lock");
		Check(Aster::FileLock::TryAcquire(file) != nullptr, "Invalid acquisition leaked no ownership");
#ifndef _WIN32
		Check(mkfifo((root / "Pipe.lock").c_str(), 0600) == 0, "Create special-file fixture");
		Rejects([&] { (void)Aster::FileLock::TryAcquire(root / "Pipe.lock"); }, "FIFO rejected without blocking");
#endif
		const auto storage = Aster::PrepareEditorStorageDirectory(root);
		Check(storage == std::filesystem::canonical(root) / ".aster", "Editor storage is canonical");
		Check(Aster::PrepareEditorStorageDirectory(root) == storage, "Existing storage remains stable");
#ifndef _WIN32
		struct stat permissions
		{
		};
		Check(stat(storage.c_str(), &permissions) == 0 && (permissions.st_mode & 0777) == 0700,
			  "New editor storage is private");
#endif
		std::filesystem::remove(storage);
		Aster::WriteTextFileAtomically(storage, "not a directory");
		Rejects([&] { (void)Aster::PrepareEditorStorageDirectory(root); }, "Storage file cannot be used as directory");
		Check(Aster::ReadTextFile(storage, 1024) == "not a directory", "Invalid storage is not overwritten");
		std::filesystem::remove(storage);
		std::filesystem::create_directory(root / "Alternate");
		std::filesystem::create_directory_symlink(root / "Alternate", storage, error);
		if (!error)
		{
			Rejects([&] { (void)Aster::PrepareEditorStorageDirectory(root); }, "Linked storage rejected");
			Check(std::filesystem::is_empty(root / "Alternate"), "Linked storage target remains unchanged");
		}
#ifndef _WIN32
		Check(!error, "Storage link test must run on Unix");
#endif
	}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		Check(argc >= 3, "Expected mode and path");
		const std::string mode = argv[1];
		const std::filesystem::path path = argv[2];
		if (mode == "unit")
		{
			TestLocks(path);
			std::cout << "passed\n";
		}
		else if (mode == "probe" || mode == "hold")
		{
			auto lock = Aster::FileLock::TryAcquire(path);
			std::cout << (lock ? "locked" : "busy") << std::endl;
			if (lock && mode == "hold")
			{
				std::string request;
				Check(static_cast<bool>(std::getline(std::cin, request)) && request == "release", "Expected release");
				lock.reset();
				std::cout << "released\n";
			}
		}
		else if (mode == "save")
		{
			Check(argc == 4, "Expected scene path and authored name");
			Aster::SceneDocumentFile document;
			auto scene = document.Load(path);
			scene.SetName(argv[3]);
			std::cout << "ready" << std::endl;
			std::string request;
			Check(static_cast<bool>(std::getline(std::cin, request)) && request == "save", "Expected save");
			try
			{
				document.Save(scene, path);
				std::cout << "saved\n";
			}
			catch (const std::runtime_error& error)
			{
				std::cout << "rejected: " << error.what() << '\n';
			}
		}
		else if (mode == "configure")
		{
			Check(argc == 4, "Expected project path and authored configuration name");
			auto project = Aster::Project::Load(path);
			auto config = project.GetConfig();
			config.Name = argv[3];
			std::cout << "ready" << std::endl;
			std::string request;
			Check(static_cast<bool>(std::getline(std::cin, request)) && request == "save", "Expected save");
			try
			{
				project.UpdateConfig(std::move(config));
				std::cout << "saved\n";
			}
			catch (const std::runtime_error& error)
			{
				std::cout << "rejected: " << error.what() << '\n';
			}
		}
		else
		{
			throw std::invalid_argument("Unknown test mode");
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
