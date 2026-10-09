#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Aster
{
	struct EditorViewport
	{
		glm::vec2 Position{0.0f};
		glm::vec2 Size{1.0f};
		glm::vec2 DisplaySize{1.0f};
	};

	// Sizes are ImGui logical coordinates, independent of framebuffer pixel scale.
	[[nodiscard]] inline EditorViewport CalculateEditorViewport(glm::vec2 displaySize)
	{
		if (!std::isfinite(displaySize.x) || !std::isfinite(displaySize.y))
		{
			throw std::invalid_argument("Editor display size must be finite");
		}
		EditorViewport viewport;
		viewport.DisplaySize = glm::max(displaySize, glm::vec2(1.0f));
		viewport.Position = glm::min(glm::vec2(260.0f, 74.0f), viewport.DisplaySize - glm::vec2(1.0f));
		const auto available = viewport.DisplaySize - viewport.Position;
		viewport.Size = glm::clamp(available - glm::vec2(330.0f, 216.0f), glm::vec2(1.0f), available);
		return viewport;
	}

	// Preserve vertical FOV and depth, correct horizontal FOV to the content area,
	// then map its clip coordinates into the full-frame render target. NVRHI handles
	// Vulkan viewport Y; positive projection Y is also what ImGuizmo consumes.
	[[nodiscard]] inline glm::mat4 FitPerspectiveToViewport(glm::mat4 projection, const EditorViewport& viewport)
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			if (!std::isfinite(viewport.Position[axis]) || !std::isfinite(viewport.Size[axis]) ||
				!std::isfinite(viewport.DisplaySize[axis]) || viewport.Position[axis] < 0.0f ||
				viewport.Size[axis] <= 0.0f ||
				viewport.DisplaySize[axis] < viewport.Position[axis] + viewport.Size[axis])
			{
				throw std::invalid_argument("Editor viewport must lie within its display");
			}
		}
		projection[0][0] = projection[1][1] * viewport.Size.y / viewport.Size.x;
		glm::mat4 placement(1.0f);
		placement[0][0] = viewport.Size.x / viewport.DisplaySize.x;
		placement[1][1] = viewport.Size.y / viewport.DisplaySize.y;
		placement[3][0] = (2.0f * viewport.Position.x + viewport.Size.x) / viewport.DisplaySize.x - 1.0f;
		placement[3][1] = 1.0f - (2.0f * viewport.Position.y + viewport.Size.y) / viewport.DisplaySize.y;
		return placement * projection;
	}
} // namespace Aster
