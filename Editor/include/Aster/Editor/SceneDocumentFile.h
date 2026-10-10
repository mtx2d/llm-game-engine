#pragma once

#include <Aster/Scene/Scene.h>

#include <filesystem>
#include <optional>
#include <string>

namespace Aster
{
	// Owns the last observed file bytes independently of authored scene/history.
	// Load and Save publish a new baseline only after validation/persistence succeeds.
	class SceneDocumentFile
	{
	  public:
		[[nodiscard]] Scene Load(const std::filesystem::path& path);
		void Save(const Scene& scene, const std::filesystem::path& path);
		void VerifyUnchanged() const;
		[[nodiscard]] const std::string& GetContents() const noexcept
		{
			return m_Contents;
		}

	  private:
		std::optional<std::filesystem::path> m_Path;
		std::string m_Contents;
	};
} // namespace Aster
