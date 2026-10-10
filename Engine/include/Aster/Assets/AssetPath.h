#pragma once

#include <filesystem>
#include <string_view>

namespace Aster
{
	// Reserved at every asset-directory depth, case-insensitively for portable
	// projects. Editor locks/recovery data must never become game assets.
	[[nodiscard]] inline bool IsEditorMetadataName(std::string_view name) noexcept
	{
		constexpr std::string_view expected = ".aster";
		if (name.size() != expected.size())
		{
			return false;
		}
		for (size_t index = 0; index < name.size(); ++index)
		{
			const char lower = name[index] >= 'A' && name[index] <= 'Z' ? name[index] + ('a' - 'A') : name[index];
			if (lower != expected[index])
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline bool ContainsEditorMetadata(const std::filesystem::path& relative)
	{
		for (const auto& segment : relative)
		{
			if (IsEditorMetadataName(segment.string()))
			{
				return true;
			}
		}
		return false;
	}
} // namespace Aster
