#pragma once

#include <filesystem>

namespace Aster
{
	struct ExportSettings
	{
		std::filesystem::path AssetRoot;
		// Relative to AssetRoot, serialized Aster scene.
		std::filesystem::path ScenePath;
		std::filesystem::path RuntimeExecutable;
		std::filesystem::path ThirdPartyNotices;
		// Must be absent or an empty directory, with an existing parent directory.
		std::filesystem::path OutputDirectory;
	};

	class ProjectExporter
	{
	  public:
		// Copies a same-platform runtime, all assets, Game.json, and notices into a
		// sibling staging directory, then publishes it through an atomic rename.
		// Serialized scene/component and glTF URI references must resolve inside
		// AssetRoot. All asset files are copied, including dynamically used Lua assets.
		// Escaping/cyclic links and special files are rejected before copying.
		// Failure leaves the destination untouched and removes staging contents.
		[[nodiscard]] static std::filesystem::path Export(const ExportSettings& settings);
	};
} // namespace Aster
