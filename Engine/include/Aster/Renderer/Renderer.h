#pragma once

#include <Aster/Input/InputState.h>

#include <array>
#include <cstdint>
#include <functional>
#include <glm/mat4x4.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nvrhi
{
	class IDevice;
	class ICommandList;
	class IFramebuffer;
} // namespace nvrhi

namespace Aster
{
	class Scene;
	class AssetImporter;
	struct EnvironmentMaps;

	struct RenderCamera
	{
		glm::mat4 View{1.0f};
		// Right-handed, zero-to-one depth; NVRHI's viewport supplies the Vulkan Y conversion.
		glm::mat4 Projection{1.0f};
	};

	struct RenderSettings
	{
		float Exposure = 1.0f;
		float AmbientIntensity = 0.03f;
		float EnvironmentIntensity = 1.0f;
		float EnvironmentRotation = 0.0f;
		bool UseSceneEnvironment = true;
		bool DrawSky = true;
		bool Shadows = true;
		uint32_t ShadowResolution = 512;
		float ShadowBias = 0.001f;
		float ShadowSoftness = 1.5f;
		bool AmbientOcclusion = true;
		float AmbientOcclusionRadius = 0.5f;
		float AmbientOcclusionBias = 0.02f;
		float AmbientOcclusionPower = 1.5f;
		std::array<float, 3> BackgroundColor{0.02f, 0.025f, 0.04f};
		std::optional<RenderCamera> CameraOverride;
	};
	struct RendererOptions
	{
		uint32_t Width = 1280;
		uint32_t Height = 720;
		bool Headless = true;
		bool EnableValidation = true;
		bool Visible = true;
		std::string Title = "Aster";
		// Empty selects a suitable GPU automatically; otherwise require an exact device name.
		std::string DeviceName;
	};

	struct RenderImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;
	};

	// All renderer and window operations belong to the constructing thread.
	// Headless rendering creates a real Vulkan device without initializing GLFW.
	// One device may be active per process; viewports share this renderer.
	class Renderer
	{
	  public:
		explicit Renderer(const RendererOptions& options = {});
		~Renderer();
		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;
		Renderer(Renderer&&) = delete;
		Renderer& operator=(Renderer&&) = delete;

		void RenderFrame(const std::array<float, 4>& clearColor = {0.04f, 0.06f, 0.1f, 1.0f});
		void RenderScene(const Scene& scene, const AssetImporter& importer, const RenderSettings& settings = {});
		void InvalidateAssets();
		void SetEnvironment(const EnvironmentMaps& environment);
		// The callback records into the open command list after scene tonemapping.
		// Pointers are borrowed for the callback duration. Clear the callback before its owner dies.
		using OverlayCallback = std::function<void(nvrhi::IDevice*, nvrhi::ICommandList*, nvrhi::IFramebuffer*)>;
		void SetOverlayCallback(OverlayCallback callback);
		void* GetNativeWindow() const;
		// Returns view and right-handed projection with zero-to-one depth.
		std::pair<glm::mat4, glm::mat4> GetSceneCamera(const Scene& scene) const;
		RenderImage ReadbackRgba8();
		void Resize(uint32_t width, uint32_t height);
		void PollEvents();
		InputSnapshot GetInputSnapshot() const;
		// Explicit shutdown permits checking diagnostics emitted during resource destruction.
		void Shutdown();
		bool ShouldClose() const;
		std::string GetDeviceName() const;
		std::vector<std::string> GetValidationMessages() const;
		uint32_t GetWidth() const;
		uint32_t GetHeight() const;

	  private:
		class Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace Aster
