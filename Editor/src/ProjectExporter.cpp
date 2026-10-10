#include <Aster/Assets/AssetImporter.h>
#include <Aster/Assets/AssetPath.h>
#include <Aster/Core/DirectoryPublication.h>
#include <Aster/Editor/ProjectExporter.h>
#include <Aster/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/loader.h>
#endif

namespace Aster
{
	namespace
	{
		using Path = std::filesystem::path;
		using Json = nlohmann::json;

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::runtime_error("Export: " + message);
			}
		}

		bool IsWithin(const Path& root, const Path& candidate)
		{
			auto rootPart = root.begin();
			auto candidatePart = candidate.begin();
			for (; rootPart != root.end(); ++rootPart, ++candidatePart)
			{
				if (candidatePart == candidate.end() || *rootPart != *candidatePart)
				{
					return false;
				}
			}
			return true;
		}

		Path ResolveFile(const Path& root, const Path& relative)
		{
			Scene::ValidateAssetPath(relative.generic_string());
			Require(!relative.empty(), "Empty asset reference");
			const auto resolved = std::filesystem::canonical(root / relative);
			Require(IsWithin(root, resolved), "Asset reference escapes root: " + relative.generic_string());
			Require(!ContainsEditorMetadata(resolved.lexically_relative(root)),
					"Asset reference resolves into reserved editor storage: " + relative.generic_string());
			Require(std::filesystem::is_regular_file(resolved),
					"Asset reference is not a file: " + relative.generic_string());
			return resolved;
		}

		struct AssetFile
		{
			Path Source;
			Path Relative;
		};

		void CollectFiles(const Path& root, const Path& directory, const Path& relative, std::set<Path>& ancestors,
						  std::vector<AssetFile>& files, std::size_t depth = 0)
		{
			Require(depth < 128, "Asset directory nesting exceeds 128 levels");
			const auto canonical = std::filesystem::canonical(directory);
			Require(IsWithin(root, canonical), "Symbolic link escapes root: " + directory.string());
			Require(ancestors.insert(canonical).second, "Cyclic asset directory link: " + directory.string());
			for (const auto& entry : std::filesystem::directory_iterator(directory))
			{
				if (IsEditorMetadataName(entry.path().filename().string()))
				{
					continue;
				}
				const auto resolved = std::filesystem::canonical(entry.path());
				Require(IsWithin(root, resolved), "Symbolic link escapes root: " + entry.path().string());
				if (ContainsEditorMetadata(resolved.lexically_relative(root)))
				{
					continue;
				}
				const auto childRelative = relative / entry.path().filename();
				if (std::filesystem::is_directory(resolved))
				{
					CollectFiles(root, entry.path(), childRelative, ancestors, files, depth + 1);
				}
				else
				{
					Require(std::filesystem::is_regular_file(resolved),
							"Special asset files are unsupported: " + entry.path().string());
					files.push_back({resolved, childRelative});
				}
			}
			ancestors.erase(canonical);
		}

		std::vector<AssetFile> CollectFiles(const Path& root)
		{
			std::vector<AssetFile> files;
			std::set<Path> ancestors;
			CollectFiles(root, root, {}, ancestors, files);
			std::sort(files.begin(), files.end(),
					  [](const auto& first, const auto& second) { return first.Relative < second.Relative; });
			return files;
		}

		Json ReadJson(const Path& path)
		{
			Require(std::filesystem::file_size(path) <= 64ULL * 1024ULL * 1024ULL,
					"JSON file exceeds 64 MiB: " + path.string());
			std::ifstream stream(path, std::ios::binary);
			Require(stream.good(), "Cannot read JSON file: " + path.string());
			Json document;
			stream >> document;
			stream >> std::ws;
			Require(stream.eof(), "Trailing data in JSON file: " + path.string());
			return document;
		}

		void ValidateSceneAssets(const Path& root, const Scene& scene)
		{
			if (!scene.GetEnvironment().Path.empty())
			{
				(void)ResolveFile(root, scene.GetEnvironment().Path);
				(void)AssetImporter(root).LoadHDR(scene.GetEnvironment().Path);
			}
			for (const auto entity : scene.Entities())
			{
				const auto& data = scene.Get(entity);
				if (data.MeshRenderer)
				{
					(void)ResolveFile(root, data.MeshRenderer->Mesh);
					(void)AssetImporter(root).LoadMesh(data.MeshRenderer->Mesh);
				}
				if (data.Script)
				{
					(void)ResolveFile(root, data.Script->Path);
				}
				if (data.AudioSource)
				{
					(void)ResolveFile(root, data.AudioSource->Path);
				}
			}
		}

		int HexDigit(char value)
		{
			if (value >= '0' && value <= '9')
			{
				return value - '0';
			}
			if (value >= 'a' && value <= 'f')
			{
				return value - 'a' + 10;
			}
			if (value >= 'A' && value <= 'F')
			{
				return value - 'A' + 10;
			}
			return -1;
		}

		std::string DecodeUri(const std::string& uri)
		{
			std::string decoded;
			for (std::size_t index = 0; index < uri.size(); ++index)
			{
				if (uri[index] != '%')
				{
					decoded.push_back(uri[index]);
					continue;
				}
				Require(index + 2 < uri.size(), "Truncated URI escape");
				const int high = HexDigit(uri[index + 1]);
				const int low = HexDigit(uri[index + 2]);
				Require(high >= 0 && low >= 0, "Invalid URI escape");
				decoded.push_back(static_cast<char>(high * 16 + low));
				index += 2;
			}
			Require(!decoded.empty() && decoded.find_first_of(":\\?#") == std::string::npos &&
						decoded.find('\0') == std::string::npos,
					"Unsupported glTF URI: " + uri);
			return decoded;
		}

		void ValidateGltfAssets(const Path& root, const AssetFile& file, const Json& document)
		{
			Require(document.is_object(), "glTF document must be an object");
			for (const char* collection : {"buffers", "images"})
			{
				if (!document.contains(collection))
				{
					continue;
				}
				Require(document.at(collection).is_array(), "glTF asset collection must be an array");
				for (const auto& item : document.at(collection))
				{
					if (!item.contains("uri"))
					{
						continue;
					}
					const auto uri = item.at("uri").get<std::string>();
					if (uri.starts_with("data:"))
					{
						continue;
					}
					const Path relative = DecodeUri(uri);
					Require(!relative.is_absolute(), "Absolute glTF URI: " + uri);
					Require(!ContainsEditorMetadata(relative), "glTF URI references reserved editor storage: " + uri);
					const auto path = std::filesystem::canonical(root / file.Relative.parent_path() / relative);
					Require(IsWithin(root, path) && std::filesystem::is_regular_file(path),
							"glTF URI escapes assets or is missing: " + uri);
					Require(!ContainsEditorMetadata(path.lexically_relative(root)),
							"glTF URI resolves into reserved editor storage: " + uri);
				}
			}
		}

		Json ReadGlb(const Path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			Require(stream.good(), "Cannot read GLB: " + path.string());
			auto readWord = [&]()
			{
				std::array<unsigned char, 4> bytes{};
				stream.read(reinterpret_cast<char*>(bytes.data()), 4);
				Require(stream.good(), "Truncated GLB: " + path.string());
				return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
					   (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
			};
			Require(readWord() == 0x46546C67, "Invalid GLB magic");
			Require(readWord() == 2, "Unsupported GLB version");
			const auto totalLength = readWord();
			Require(totalLength == std::filesystem::file_size(path), "GLB length does not match file");
			const auto jsonLength = readWord();
			Require(readWord() == 0x4E4F534A, "First GLB chunk must be JSON");
			Require(totalLength >= 20 && jsonLength <= totalLength - 20 && jsonLength <= 64 * 1024 * 1024,
					"Invalid GLB JSON chunk length");
			std::string source(jsonLength, '\0');
			stream.read(source.data(), static_cast<std::streamsize>(source.size()));
			Require(stream.good(), "Truncated GLB JSON chunk");
			return Json::parse(source);
		}

		void ValidateFiles(const Path& root, const std::vector<AssetFile>& files)
		{
			for (const auto& file : files)
			{
				const auto extension = file.Relative.extension().string();
				if (extension == ".aster")
				{
					ValidateSceneAssets(root, Scene::Load(file.Source));
				}
				else if (extension == ".json")
				{
					const auto document = ReadJson(file.Source);
					if (document.contains("Entities"))
					{
						ValidateSceneAssets(root, Scene::Deserialize(document));
					}
				}
				else if (extension == ".gltf")
				{
					ValidateGltfAssets(root, file, ReadJson(file.Source));
				}
				else if (extension == ".glb")
				{
					ValidateGltfAssets(root, file, ReadGlb(file.Source));
				}
			}
		}

		void CopyFiles(const std::vector<AssetFile>& files, const Path& destination)
		{
			std::filesystem::create_directories(destination);
			for (const auto& file : files)
			{
				const auto output = destination / file.Relative;
				std::filesystem::create_directories(output.parent_path());
				std::filesystem::copy_file(file.Source, output);
			}
		}

#ifdef __APPLE__
		bool IsSupportedDriverVersion(const std::string& version)
		{
			std::array<std::uint32_t, 3> parts{};
			std::size_t offset = 0;
			for (std::size_t index = 0; index < parts.size(); ++index)
			{
				const auto end = version.find('.', offset);
				if ((index + 1 == parts.size()) != (end == std::string::npos))
				{
					return false;
				}
				const auto length = end == std::string::npos ? version.size() - offset : end - offset;
				const auto parsed =
					std::from_chars(version.data() + offset, version.data() + offset + length, parts[index]);
				if (parsed.ec != std::errc{} || parsed.ptr != version.data() + offset + length)
				{
					return false;
				}
				offset += length + 1;
			}
			return parts[0] <= 127 && parts[1] <= 1023 && parts[2] <= 4095 &&
				   (parts[0] > 1 || (parts[0] == 1 && parts[1] >= 3));
		}

		void ValidateCompanion(const Path& path, std::uintmax_t maximumSize)
		{
			Require(std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path),
					"Missing or nonregular macOS runtime companion: " + path.string());
			const auto size = std::filesystem::file_size(path);
			Require(size > 0 && size <= maximumSize, "Invalid macOS runtime companion size: " + path.string());
		}

		bool RequiresVulkanBundle(const Path& runtime)
		{
			// Inspect the actual supplied executable, so a deleted companion folder
			// cannot turn a graphical runtime into an apparently valid CPU package.
			std::ifstream stream(runtime, std::ios::binary);
			mach_header_64 header{};
			stream.read(reinterpret_cast<char*>(&header), sizeof(header));
			Require(stream.good() && header.magic == MH_MAGIC_64 && header.filetype == MH_EXECUTE,
					"Runtime must be a native 64-bit Mach-O executable (universal archives are not supported)");
			Require(header.ncmds > 0 && header.sizeofcmds <= 1024 * 1024 &&
						header.ncmds <= header.sizeofcmds / sizeof(load_command),
					"Invalid Mach-O runtime load-command bounds");
			std::vector<char> commands(header.sizeofcmds);
			stream.read(commands.data(), static_cast<std::streamsize>(commands.size()));
			Require(stream.good(), "Truncated Mach-O runtime load commands");
			bool requiresVulkan = false;
			bool hasBundleRPath = false;
			std::size_t offset = 0;
			for (std::uint32_t index = 0; index < header.ncmds; ++index)
			{
				Require(offset <= commands.size() && commands.size() - offset >= sizeof(load_command),
						"Truncated Mach-O runtime load command");
				load_command command{};
				std::memcpy(&command, commands.data() + offset, sizeof(command));
				Require(command.cmdsize >= sizeof(command) && command.cmdsize <= commands.size() - offset,
						"Invalid Mach-O runtime load-command size");
				const auto readString = [&](std::uint32_t start, std::size_t minimum)
				{
					Require(start >= minimum && start < command.cmdsize, "Invalid Mach-O runtime string offset");
					const auto first = commands.begin() + static_cast<std::ptrdiff_t>(offset + start);
					const auto last = commands.begin() + static_cast<std::ptrdiff_t>(offset + command.cmdsize);
					const auto end = std::find(first, last, '\0');
					Require(end != last, "Unterminated Mach-O runtime string");
					return std::string(first, end);
				};
				if (command.cmd == LC_LOAD_DYLIB || command.cmd == LC_LOAD_WEAK_DYLIB ||
					command.cmd == LC_REEXPORT_DYLIB || command.cmd == LC_LOAD_UPWARD_DYLIB ||
					command.cmd == LC_LAZY_LOAD_DYLIB)
				{
					Require(command.cmdsize >= sizeof(dylib_command), "Truncated Mach-O dylib command");
					dylib_command library{};
					std::memcpy(&library, commands.data() + offset, sizeof(library));
					const auto name = readString(library.dylib.name.offset, sizeof(library));
					if (name == "@rpath/libvulkan.1.dylib")
					{
						requiresVulkan = true;
					}
					else
					{
						Require(name.starts_with("/usr/lib/") || name.starts_with("/System/Library/"),
								"Unsupported non-system runtime library; rebuild with portable dependencies: " + name);
					}
				}
				else if (command.cmd == LC_RPATH)
				{
					Require(command.cmdsize >= sizeof(rpath_command), "Truncated Mach-O rpath command");
					rpath_command path{};
					std::memcpy(&path, commands.data() + offset, sizeof(path));
					const auto value = readString(path.path.offset, sizeof(path));
					hasBundleRPath = hasBundleRPath || value == "@executable_path/../Frameworks";
					Require(value == "@executable_path/../Frameworks" ||
								value == "@executable_path/AsterRuntimeDependencies",
							"Runtime embeds a nonportable library search path: " + value);
				}
				offset += command.cmdsize;
			}
			Require(offset == commands.size(), "Mach-O runtime command count does not match its byte length");
			Require(!requiresVulkan || hasBundleRPath,
					"Graphical runtime lacks @executable_path/../Frameworks; rebuild with macOS export support");
			return requiresVulkan;
		}

		void WriteMacInfo(const Path& contents)
		{
			std::ofstream plist(contents / "Info.plist", std::ios::binary);
			plist.exceptions(std::ios::badbit | std::ios::failbit);
			plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
					 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
					 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
					 "<plist version=\"1.0\"><dict>\n"
					 "<key>CFBundleExecutable</key><string>AsterGame</string>\n"
					 "<key>CFBundleIdentifier</key><string>org.asterengine.exported-game</string>\n"
					 "<key>CFBundleName</key><string>Aster Game</string>\n"
					 "<key>CFBundlePackageType</key><string>APPL</string>\n"
					 "<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
					 "<key>CFBundleShortVersionString</key><string>1.0</string>\n"
					 "<key>CFBundleVersion</key><string>1</string>\n"
					 "<key>NSHighResolutionCapable</key><true/>\n"
					 "</dict></plist>\n";
			plist.close();
		}
#endif
	} // namespace

	std::filesystem::path ProjectExporter::Export(const ExportSettings& settings)
	{
		Require(!settings.OutputDirectory.empty(), "Output directory is empty");
		const auto root = std::filesystem::canonical(settings.AssetRoot);
		Require(std::filesystem::is_directory(root), "Asset root is not a directory");
		const auto output = std::filesystem::weakly_canonical(settings.OutputDirectory);
		Require(!IsWithin(root, output), "Output directory must be outside the asset tree");
		Require(std::filesystem::is_directory(output.parent_path()), "Output parent directory does not exist");
		Require(!std::filesystem::exists(output) ||
					(std::filesystem::is_directory(output) && std::filesystem::is_empty(output)),
				"Output directory must be absent or empty");
		const auto runtime = std::filesystem::canonical(settings.RuntimeExecutable);
		Require(std::filesystem::is_regular_file(runtime), "Runtime executable is not a file");
#if defined(_WIN32)
		// A runtime supplied with an application-local Vulkan loader must retain
		// that dependency when moved. A system-installed loader remains supported.
		const auto vulkanLoader = runtime.parent_path() / "vulkan-1.dll";
		const bool hasVulkanLoader = std::filesystem::exists(vulkanLoader) || std::filesystem::is_symlink(vulkanLoader);
		if (hasVulkanLoader)
		{
			Require(std::filesystem::is_regular_file(vulkanLoader) && !std::filesystem::is_symlink(vulkanLoader),
					"Companion vulkan-1.dll must be a regular file without a symbolic link");
			const auto loaderSize = std::filesystem::file_size(vulkanLoader);
			Require(loaderSize > 0 && loaderSize <= 256ULL * 1024ULL * 1024ULL,
					"Companion vulkan-1.dll size must be in (0,256 MiB]");
		}
#else
		const auto executePermissions = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
										std::filesystem::perms::others_exec;
		Require((std::filesystem::status(runtime).permissions() & executePermissions) != std::filesystem::perms::none,
				"Runtime file has no executable permission");
#endif
#ifdef __APPLE__
		const bool hasVulkanBundle = RequiresVulkanBundle(runtime);
		const auto companions = runtime.parent_path() / "AsterRuntimeDependencies";
		Json moltenManifest;
		if (hasVulkanBundle)
		{
			Require(std::filesystem::is_directory(companions) && !std::filesystem::is_symlink(companions),
					"Graphical runtime requires its adjacent AsterRuntimeDependencies directory");
			for (const char* name : {"libvulkan.1.dylib", "libMoltenVK.dylib"})
			{
				ValidateCompanion(companions / name, 256ULL * 1024ULL * 1024ULL);
			}
			ValidateCompanion(companions / "MoltenVK-LICENSE.txt", 1024 * 1024);
			ValidateCompanion(companions / "MoltenVK_icd.json", 65536);
			moltenManifest = ReadJson(companions / "MoltenVK_icd.json");
			Require(moltenManifest.is_object() && moltenManifest.value("file_format_version", "") == "1.0.0" &&
						moltenManifest.contains("ICD") && moltenManifest.at("ICD").is_object(),
					"Invalid MoltenVK driver manifest");
			auto& driver = moltenManifest.at("ICD");
			Require(driver.value("is_portability_driver", false) && driver.contains("api_version") &&
						driver.at("api_version").is_string() &&
						IsSupportedDriverVersion(driver.at("api_version").get<std::string>()),
					"MoltenVK manifest must describe a portability driver with a valid Vulkan version >= 1.3.0");
			driver["library_path"] = "../../../Frameworks/libMoltenVK.dylib";
		}
#endif
		const auto notices = std::filesystem::canonical(settings.ThirdPartyNotices);
		Require(std::filesystem::is_directory(notices), "Third-party notices must be a directory");
#if defined(_WIN32)
		if (hasVulkanLoader)
		{
			Require(std::filesystem::is_regular_file(notices / "Licenses/vulkan_loader.txt") &&
						std::filesystem::is_regular_file(notices / "Licenses/vulkan_loader_notices.txt"),
					"Bundled vulkan-1.dll requires Vulkan loader license and permissive notices");
		}
#endif

#ifdef __APPLE__
		if (hasVulkanBundle)
		{
			ValidateCompanion(notices / "Licenses/vulkan_loader.txt", 1024 * 1024);
			ValidateCompanion(notices / "Licenses/vulkan_loader_notices.txt", 1024 * 1024);
		}
#endif
		const auto scenePath = ResolveFile(root, settings.ScenePath);
		ValidateSceneAssets(root, Scene::Load(scenePath));
		const auto assetFiles = CollectFiles(root);
		const auto noticeFiles = CollectFiles(notices);
		Require(!noticeFiles.empty(), "Third-party notices are empty");
		ValidateFiles(root, assetFiles);

		std::random_device random;
		Path staging;
		for (int attempt = 0; attempt < 20; ++attempt)
		{
			staging = output.parent_path() / (output.filename().string() + ".aster-export-" + std::to_string(random()) +
											  "-" + std::to_string(random()));
			if (std::filesystem::create_directory(staging))
			{
				break;
			}
			staging.clear();
		}
		Require(!staging.empty(), "Cannot create staging directory");
		bool removedEmptyDestination = false;
		try
		{
			auto resources = staging;
			auto binaries = staging;
#ifdef __APPLE__
			const auto contents = staging / "AsterGame.app/Contents";
			resources = contents / "Resources";
			binaries = contents / "MacOS";
			std::filesystem::create_directories(binaries);
			WriteMacInfo(contents);
#endif
			CopyFiles(assetFiles, resources / "Assets");
			CopyFiles(noticeFiles, resources / "ThirdParty");
#if defined(_WIN32)
			const auto executableName = "AsterGame.exe";
#else
			const auto executableName = "AsterGame";
#endif
			std::filesystem::copy_file(runtime, binaries / executableName);
#if defined(_WIN32)
			if (hasVulkanLoader)
			{
				std::filesystem::copy_file(vulkanLoader, staging / "vulkan-1.dll");
			}
#endif
#ifdef __APPLE__
			if (hasVulkanBundle)
			{
				std::filesystem::create_directory(contents / "Frameworks");
				for (const char* name : {"libvulkan.1.dylib", "libMoltenVK.dylib"})
				{
					std::filesystem::copy_file(companions / name, contents / "Frameworks" / name);
				}
				std::filesystem::copy_file(companions / "MoltenVK-LICENSE.txt",
										   resources / "ThirdParty/Licenses/MoltenVK.txt",
										   std::filesystem::copy_options::overwrite_existing);
				const auto driverDirectory = resources / "vulkan/icd.d";
				std::filesystem::create_directories(driverDirectory);
				std::ofstream driver(driverDirectory / "MoltenVK_icd.json", std::ios::binary);
				driver.exceptions(std::ios::badbit | std::ios::failbit);
				driver << moltenManifest.dump(2) << '\n';
				driver.close();
			}
#endif
			std::ofstream manifest(resources / "Game.json", std::ios::binary);
			manifest.exceptions(std::ios::badbit | std::ios::failbit);
			manifest
				<< Json({{"Version", 1}, {"Scene", settings.ScenePath.generic_string()}, {"Assets", "Assets"}}).dump(2)
				<< '\n';
			manifest.close();
			// Revalidate the staged bytes, including all serialized file references.
			ValidateFiles(resources / "Assets", CollectFiles(resources / "Assets"));
			Require(!std::filesystem::exists(output) ||
						(std::filesystem::is_directory(output) && std::filesystem::is_empty(output)),
					"Output directory changed during export");
			if (std::filesystem::exists(output))
			{
				removedEmptyDestination = RemoveEmptyDirectoryForPublication(output);
			}
			PublishDirectoryExclusively(staging, output);
			return output;
		}
		catch (...)
		{
			std::error_code cleanupError;
			std::filesystem::remove_all(staging, cleanupError);
			std::error_code restoreError;
			if (removedEmptyDestination && !std::filesystem::exists(output, restoreError) && !restoreError)
			{
				std::filesystem::create_directory(output, restoreError);
			}
			if (restoreError)
			{
				std::throw_with_nested(std::runtime_error("Export failed; cannot restore empty destination: " +
														  output.string() + ": " + restoreError.message()));
			}
			if (cleanupError)
			{
				std::throw_with_nested(std::runtime_error("Export failed; cannot clean staging directory: " +
														  staging.string() + ": " + cleanupError.message()));
			}
			throw;
		}
	}
} // namespace Aster
