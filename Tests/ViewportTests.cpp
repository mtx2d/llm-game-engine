#include <Aster/Editor/ViewportCamera.h>

#include <glm/ext/matrix_clip_space.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error("Viewport: " + message);
		}
	}

	bool Near(float first, float second)
	{
		return std::abs(first - second) < 0.001f;
	}

	glm::vec3 Project(const glm::mat4& projection, glm::vec3 point, glm::vec2 framebufferSize)
	{
		const auto clip = projection * glm::vec4(point, 1.0f);
		const auto ndc = glm::vec3(clip) / clip.w;
		return {(ndc.x + 1.0f) * 0.5f * framebufferSize.x, (1.0f - ndc.y) * 0.5f * framebufferSize.y, ndc.z};
	}

	void TestContentProjection(glm::vec2 displaySize)
	{
		const auto viewport = Aster::CalculateEditorViewport(displaySize);
		const auto projection = Aster::FitPerspectiveToViewport(
			glm::perspectiveRH_ZO(glm::radians(60.0f), displaySize.x / displaySize.y, 0.1f, 100.0f), viewport);
		const auto center = Project(projection, {0, 0, -5}, displaySize);
		Check(Near(center.x, viewport.Position.x + viewport.Size.x / 2) &&
				  Near(center.y, viewport.Position.y + viewport.Size.y / 2),
			  "Camera center falls beneath editor panels");
		const float halfHeight = 5.0f * std::tan(glm::radians(30.0f));
		const float halfWidth = halfHeight * viewport.Size.x / viewport.Size.y;
		const auto topLeft = Project(projection, {-halfWidth, halfHeight, -5}, displaySize);
		const auto bottomRight = Project(projection, {halfWidth, -halfHeight, -5}, displaySize);
		Check(Near(topLeft.x, viewport.Position.x) && Near(topLeft.y, viewport.Position.y),
			  "View's upper corner must touch the content rectangle");
		Check(Near(bottomRight.x, viewport.Position.x + viewport.Size.x) &&
				  Near(bottomRight.y, viewport.Position.y + viewport.Size.y),
			  "View's lower corner is hidden by assets");
		const auto right = Project(projection, {1, 0, -5}, displaySize);
		const auto up = Project(projection, {0, 1, -5}, displaySize);
		Check(Near(right.x - center.x, center.y - up.y), "Square geometry is stretched by viewport aspect");
		Check(Near(Project(projection, {0, 0, -0.1f}, displaySize).z, 0) &&
				  Near(Project(projection, {0, 0, -100}, displaySize).z, 1),
			  "Viewport mapping changes near/far depth");
		const auto highDpiCenter = Project(projection, {0, 0, -5}, displaySize * 2.0f);
		Check(Near(highDpiCenter.x, center.x * 2) && Near(highDpiCenter.y, center.y * 2),
			  "Logical viewport placement must scale with framebuffer pixels");
	}
} // namespace

void RunViewportTests()
{
	TestContentProjection({1440, 900});
	TestContentProjection({1024, 768});
	TestContentProjection({1920, 1080});
	const auto normal = Aster::CalculateEditorViewport({1440, 900});
	Check(normal.Position == glm::vec2(260, 74) && normal.Size == glm::vec2(850, 610),
		  "Content bounds must match hierarchy, toolbar, inspector and asset panels");
	for (const auto size : {glm::vec2(0), glm::vec2(1), glm::vec2(300, 200)})
	{
		const auto viewport = Aster::CalculateEditorViewport(size);
		const auto projection =
			Aster::FitPerspectiveToViewport(glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f), viewport);
		const auto point = Project(projection, {0, 0, -1}, viewport.DisplaySize);
		Check(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
			  "Tiny or minimized window produces a nonfinite camera");
		Check(point.x >= 0 && point.y >= 0 && point.x <= viewport.DisplaySize.x && point.y <= viewport.DisplaySize.y,
			  "Tiny-window camera center must remain within the display");
	}
	bool rejected = false;
	try
	{
		(void)Aster::CalculateEditorViewport({std::numeric_limits<float>::infinity(), 900});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Check(rejected, "Nonfinite display size must be rejected");
}
