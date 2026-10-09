#include <Aster/Editor/GuiRenderer.h>
#include <AsterShaders/GuiFrag.h>
#include <AsterShaders/GuiVert.h>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Aster
{
	namespace
	{
		void RequireResource(bool created, const char* name)
		{
			if (!created)
			{
				throw std::runtime_error(std::string("Failed to create editor GUI ") + name);
			}
		}
	} // namespace

	void GuiRenderer::Initialize(nvrhi::IDevice* device, nvrhi::ICommandList* commands,
								 nvrhi::IFramebuffer* framebuffer)
	{
		unsigned char* pixels = nullptr;
		int width = 0;
		int height = 0;
		ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		nvrhi::TextureDesc texture;
		texture.width = static_cast<uint32_t>(width);
		texture.height = static_cast<uint32_t>(height);
		texture.format = nvrhi::Format::RGBA8_UNORM;
		texture.initialState = nvrhi::ResourceStates::ShaderResource;
		texture.keepInitialState = true;
		texture.debugName = "Editor font atlas";
		m_Font = device->createTexture(texture);
		RequireResource(m_Font != nullptr, "font texture");
		commands->writeTexture(m_Font, 0, 0, pixels, static_cast<size_t>(width) * 4);
		ImGui::GetIO().Fonts->SetTexID(1);
		nvrhi::BufferDesc constantBuffer;
		constantBuffer.byteSize = 16;
		constantBuffer.isConstantBuffer = true;
		constantBuffer.isVolatile = true;
		constantBuffer.maxVersions = 16;
		constantBuffer.debugName = "Editor projection";
		m_Constants = device->createBuffer(constantBuffer);
		RequireResource(m_Constants != nullptr, "projection buffer");
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
						   nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
		m_Layout = device->createBindingLayout(layout);
		RequireResource(m_Layout != nullptr, "binding layout");
		auto sampler = device->createSampler(
			nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		RequireResource(sampler != nullptr, "font sampler");
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants),
							 nvrhi::BindingSetItem::Texture_SRV(0, m_Font), nvrhi::BindingSetItem::Sampler(0, sampler)};
		m_Bindings = device->createBindingSet(bindings, m_Layout);
		RequireResource(m_Bindings != nullptr, "font bindings");
		nvrhi::ShaderDesc vertexDescription;
		vertexDescription.shaderType = nvrhi::ShaderType::Vertex;
		vertexDescription.debugName = "Editor GUI vertex shader";
		nvrhi::ShaderDesc pixelDescription;
		pixelDescription.shaderType = nvrhi::ShaderType::Pixel;
		pixelDescription.debugName = "Editor GUI pixel shader";
		auto vertexShader = device->createShader(vertexDescription, Shaders::GuiVert, sizeof(Shaders::GuiVert));
		auto pixelShader = device->createShader(pixelDescription, Shaders::GuiFrag, sizeof(Shaders::GuiFrag));
		RequireResource(vertexShader != nullptr && pixelShader != nullptr, "shaders");
		nvrhi::VertexAttributeDesc attributes[3];
		attributes[0]
			.setName("POSITION")
			.setFormat(nvrhi::Format::RG32_FLOAT)
			.setOffset(offsetof(ImDrawVert, pos))
			.setElementStride(sizeof(ImDrawVert));
		attributes[1]
			.setName("TEXCOORD")
			.setFormat(nvrhi::Format::RG32_FLOAT)
			.setOffset(offsetof(ImDrawVert, uv))
			.setElementStride(sizeof(ImDrawVert));
		attributes[2]
			.setName("COLOR")
			.setFormat(nvrhi::Format::RGBA8_UNORM)
			.setOffset(offsetof(ImDrawVert, col))
			.setElementStride(sizeof(ImDrawVert));
		nvrhi::GraphicsPipelineDesc pipeline;
		pipeline.VS = vertexShader;
		pipeline.PS = pixelShader;
		pipeline.inputLayout = device->createInputLayout(attributes, 3, vertexShader);
		RequireResource(pipeline.inputLayout != nullptr, "vertex layout");
		pipeline.bindingLayouts = {m_Layout};
		pipeline.renderState.depthStencilState.depthTestEnable = false;
		pipeline.renderState.depthStencilState.depthWriteEnable = false;
		pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
		pipeline.renderState.rasterState.scissorEnable = true;
		auto& blend = pipeline.renderState.blendState.targets[0];
		blend.blendEnable = true;
		blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
		blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		blend.srcBlendAlpha = nvrhi::BlendFactor::One;
		blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		m_Pipeline = device->createGraphicsPipeline(pipeline, framebuffer->getFramebufferInfo());
		RequireResource(m_Pipeline != nullptr, "pipeline");
	}

	void GuiRenderer::Draw(nvrhi::IDevice* device, nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer,
						   ImDrawData* data)
	{
		if (!data || data->TotalVtxCount == 0 || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
		{
			return;
		}
		if (!m_Pipeline)
		{
			Initialize(device, commands, framebuffer);
		}
		const size_t vertexBytes = static_cast<size_t>(data->TotalVtxCount) * sizeof(ImDrawVert);
		const size_t indexBytes = static_cast<size_t>(data->TotalIdxCount) * sizeof(ImDrawIdx);
		if (!m_Vertices || m_Vertices->getDesc().byteSize < vertexBytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = vertexBytes + 65536;
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			m_Vertices = device->createBuffer(desc);
			RequireResource(m_Vertices != nullptr, "vertex buffer");
		}
		if (!m_Indices || m_Indices->getDesc().byteSize < indexBytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = indexBytes + 16384;
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			m_Indices = device->createBuffer(desc);
			RequireResource(m_Indices != nullptr, "index buffer");
		}
		size_t vertexOffset = 0;
		size_t indexOffset = 0;
		m_VertexUpload.resize(vertexBytes);
		m_IndexUpload.resize(indexBytes);
		for (const auto* list : data->CmdLists)
		{
			const size_t listVertexBytes = static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
			const size_t listIndexBytes = static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
			if (listVertexBytes != 0)
			{
				std::memcpy(m_VertexUpload.data() + vertexOffset, list->VtxBuffer.Data, listVertexBytes);
			}
			if (listIndexBytes != 0)
			{
				std::memcpy(m_IndexUpload.data() + indexOffset, list->IdxBuffer.Data, listIndexBytes);
			}
			vertexOffset += listVertexBytes;
			indexOffset += listIndexBytes;
		}
		// A single upload avoids overlapping aligned transfers at odd 16-bit index boundaries.
		commands->writeBuffer(m_Vertices, m_VertexUpload.data(), vertexBytes);
		if (indexBytes != 0)
		{
			commands->writeBuffer(m_Indices, m_IndexUpload.data(), indexBytes);
		}
		const float constants[4] = {2.0f / data->DisplaySize.x, -2.0f / data->DisplaySize.y,
									-1.0f - data->DisplayPos.x * 2.0f / data->DisplaySize.x,
									1.0f + data->DisplayPos.y * 2.0f / data->DisplaySize.y};
		commands->writeBuffer(m_Constants, constants, sizeof(constants));
		uint32_t startVertex = 0;
		uint32_t startIndex = 0;
		const auto& framebufferInfo = framebuffer->getFramebufferInfo();
		for (const auto* list : data->CmdLists)
		{
			for (const auto& draw : list->CmdBuffer)
			{
				if (draw.UserCallback)
				{
					if (draw.UserCallback != ImDrawCallback_ResetRenderState)
					{
						draw.UserCallback(list, &draw);
					}
					continue;
				}
				const float left = (draw.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x;
				const float top = (draw.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y;
				const float right = (draw.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x;
				const float bottom = (draw.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y;
				if (right <= left || bottom <= top)
				{
					continue;
				}
				if (draw.GetTexID() != 1)
				{
					throw std::runtime_error("Editor received an unregistered GUI texture");
				}
				nvrhi::GraphicsState state;
				state.pipeline = m_Pipeline;
				state.framebuffer = framebuffer;
				state.bindings = {m_Bindings};
				state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_Vertices).setSlot(0).setOffset(0)};
				state.indexBuffer =
					nvrhi::IndexBufferBinding()
						.setBuffer(m_Indices)
						.setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
						.setOffset(0);
				state.viewport.addViewport(framebufferInfo.getViewport());
				state.viewport.addScissorRect(nvrhi::Rect(
					static_cast<int>(std::clamp(left, 0.0f, static_cast<float>(framebufferInfo.width))),
					static_cast<int>(std::clamp(right, 0.0f, static_cast<float>(framebufferInfo.width))),
					static_cast<int>(std::clamp(top, 0.0f, static_cast<float>(framebufferInfo.height))),
					static_cast<int>(std::clamp(bottom, 0.0f, static_cast<float>(framebufferInfo.height)))));
				commands->setGraphicsState(state);
				commands->drawIndexed(nvrhi::DrawArguments()
										  .setVertexCount(draw.ElemCount)
										  .setStartIndexLocation(startIndex + draw.IdxOffset)
										  .setStartVertexLocation(startVertex + draw.VtxOffset));
			}
			startVertex += static_cast<uint32_t>(list->VtxBuffer.Size);
			startIndex += static_cast<uint32_t>(list->IdxBuffer.Size);
		}
	}
} // namespace Aster
