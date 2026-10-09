#pragma once

#include <cstddef>
#include <nvrhi/nvrhi.h>
#include <vector>

struct ImDrawData;

namespace Aster
{
	class GuiRenderer
	{
	  public:
		void Draw(nvrhi::IDevice* device, nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer,
				  ImDrawData* data);

	  private:
		void Initialize(nvrhi::IDevice* device, nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer);
		nvrhi::TextureHandle m_Font;
		nvrhi::BufferHandle m_Constants;
		nvrhi::BufferHandle m_Vertices;
		nvrhi::BufferHandle m_Indices;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
		std::vector<std::byte> m_VertexUpload;
		std::vector<std::byte> m_IndexUpload;
	};
} // namespace Aster
