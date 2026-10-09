#pragma once

#include <nvrhi/nvrhi.h>

#include <glm/mat4x4.hpp>
#include <memory>
#include <utility>

namespace Aster
{
	class Scene;
	class AssetImporter;
	struct RenderSettings;
	struct EnvironmentMaps;
	std::pair<glm::mat4, glm::mat4> CalculateSceneCamera(const Scene& scene, uint32_t width, uint32_t height);

	// Owns mesh/material GPU resources and records the HDR scene and tonemap passes.
	class SceneRenderPass
	{
	  public:
		explicit SceneRenderPass(nvrhi::IDevice* device);
		~SceneRenderPass();
		SceneRenderPass(const SceneRenderPass&) = delete;
		SceneRenderPass& operator=(const SceneRenderPass&) = delete;
		void Prepare(const Scene& scene, const AssetImporter& importer, nvrhi::ITexture* output,
					 const RenderSettings& settings);
		void Record(nvrhi::ICommandList* commands);
		void InvalidateAssets();
		void SetEnvironment(const EnvironmentMaps& environment);

	  private:
		class Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace Aster
