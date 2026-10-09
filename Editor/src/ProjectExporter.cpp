#include <Aster/Assets/AssetImporter.h>
#include <Aster/Editor/ProjectExporter.h>
#include <Aster/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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
				const auto resolved = std::filesystem::canonical(entry.path());
				Require(IsWithin(root, resolved), "Symbolic link escapes root: " + entry.path().string());
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
					const auto path = std::filesystem::canonical(root / file.Relative.parent_path() / relative);
					Require(IsWithin(root, path) && std::filesystem::is_regular_file(path),
							"glTF URI escapes assets or is missing: " + uri);
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
			CopyFiles(assetFiles, staging / "Assets");
			CopyFiles(noticeFiles, staging / "ThirdParty");
#if defined(_WIN32)
			const auto executableName = "AsterGame.exe";
#else
			const auto executableName = "AsterGame";
#endif
			std::filesystem::copy_file(runtime, staging / executableName);
#if defined(_WIN32)
			if (hasVulkanLoader)
			{
				std::filesystem::copy_file(vulkanLoader, staging / "vulkan-1.dll");
			}
#endif
			std::ofstream manifest(staging / "Game.json", std::ios::binary);
			manifest.exceptions(std::ios::badbit | std::ios::failbit);
			manifest
				<< Json({{"Version", 1}, {"Scene", settings.ScenePath.generic_string()}, {"Assets", "Assets"}}).dump(2)
				<< '\n';
			manifest.close();
			// Revalidate the staged bytes, including all serialized file references.
			ValidateFiles(staging / "Assets", CollectFiles(staging / "Assets"));
			Require(!std::filesystem::exists(output) ||
						(std::filesystem::is_directory(output) && std::filesystem::is_empty(output)),
					"Output directory changed during export");
			if (std::filesystem::exists(output))
			{
				removedEmptyDestination = std::filesystem::remove(output);
			}
			std::filesystem::rename(staging, output);
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
