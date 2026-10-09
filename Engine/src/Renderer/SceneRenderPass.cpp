#include "SceneRenderPass.h"

#include <Aster/Assets/AssetImporter.h>
#include <Aster/Renderer/Environment.h>
#include <Aster/Renderer/Renderer.h>
#include <Aster/Scene/Scene.h>
#include <AsterShaders/DepthFrag.h>
#include <AsterShaders/DepthVert.h>
#include <AsterShaders/FullscreenVert.h>
#include <AsterShaders/PbrFrag.h>
#include <AsterShaders/PbrVert.h>
#include <AsterShaders/ShadowVert.h>
#include <AsterShaders/SkyFrag.h>
#include <AsterShaders/SsaoBlurFrag.h>
#include <AsterShaders/SsaoFrag.h>
#include <AsterShaders/TonemapFrag.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace Aster
{
	namespace
	{
		struct LightConstants
		{
			glm::vec4 PositionType{0.0f};
			glm::vec4 DirectionRange{0.0f};
			glm::vec4 ColorIntensity{0.0f};
			glm::vec4 Cone{0.0f};
			std::array<glm::mat4, 6> ShadowMatrices{};
		};

		struct FrameConstants
		{
			glm::mat4 ViewProjection{1.0f};
			glm::vec4 CameraPosition{0.0f};
			glm::vec4 Parameters{0.0f};
			glm::vec4 Environment{0.0f};
			glm::vec4 Shadow{0.0f};
			std::array<LightConstants, 32> Lights;
		};

		struct OcclusionConstants
		{
			glm::mat4 Projection{1.0f};
			glm::mat4 InverseProjection{1.0f};
			glm::vec4 Parameters{0.0f};
			glm::vec4 Size{0.0f};
		};
		static_assert(sizeof(OcclusionConstants) == 160);

		struct MaterialConstants
		{
			glm::vec4 BaseColor{1.0f};
			glm::vec4 EmissiveMetallic{0.0f};
			glm::vec4 Parameters{0.0f};
			std::array<glm::vec4, 15> Transforms{};
			std::array<glm::vec4, 5> TextureInfo{};
		};

		struct DrawConstants
		{
			glm::mat4 Model{1.0f};
			glm::vec4 Color{1.0f};
			glm::vec4 Material{1.0f};
		};

		static_assert(sizeof(FrameConstants) == 128 + 32 * 448);
		static_assert(sizeof(MaterialConstants) == 368);
		static_assert(sizeof(DrawConstants) == 96);

		void RequireResource(bool available, const char* description)
		{
			if (!available)
			{
				throw std::runtime_error(std::string("NVRHI failed to create ") + description);
			}
		}

		bool IsFinite(const glm::mat4& value)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(value[column][row]))
					{
						return false;
					}
				}
			}
			return true;
		}

		ImageAsset DownsampleMaterialTexture(const ImageAsset& source, bool srgb)
		{
			ImageAsset result;
			result.Width = std::max(1u, source.Width / 2);
			result.Height = std::max(1u, source.Height / 2);
			result.Pixels.resize(static_cast<size_t>(result.Width) * result.Height * 4);
			for (uint32_t y = 0; y < result.Height; ++y)
			{
				for (uint32_t x = 0; x < result.Width; ++x)
				{
					const float left = static_cast<float>(x) * source.Width / result.Width;
					const float right = static_cast<float>(x + 1) * source.Width / result.Width;
					const float top = static_cast<float>(y) * source.Height / result.Height;
					const float bottom = static_cast<float>(y + 1) * source.Height / result.Height;
					std::array<float, 4> sum{};
					float totalWeight = 0;
					for (uint32_t sy = static_cast<uint32_t>(top);
						 sy < std::min(source.Height, static_cast<uint32_t>(std::ceil(bottom))); ++sy)
					{
						for (uint32_t sx = static_cast<uint32_t>(left);
							 sx < std::min(source.Width, static_cast<uint32_t>(std::ceil(right))); ++sx)
						{
							const float weight =
								(std::min(right, static_cast<float>(sx + 1)) - std::max(left, static_cast<float>(sx))) *
								(std::min(bottom, static_cast<float>(sy + 1)) - std::max(top, static_cast<float>(sy)));
							totalWeight += weight;
							for (size_t channel = 0; channel < 4; ++channel)
							{
								float value =
									source.Pixels[(static_cast<size_t>(sy) * source.Width + sx) * 4 + channel] / 255.0f;
								if (srgb && channel < 3)
								{
									value =
										value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
								}
								sum[channel] += value * weight;
							}
						}
					}
					for (size_t channel = 0; channel < 4; ++channel)
					{
						float value = sum[channel] / totalWeight;
						if (srgb && channel < 3)
						{
							value =
								value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
						}
						result.Pixels[(static_cast<size_t>(y) * result.Width + x) * 4 + channel] =
							static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
					}
				}
			}
			return result;
		}

		nvrhi::SamplerAddressMode AddressMode(int gltfMode)
		{
			switch (gltfMode)
			{
			case 10497:
				return nvrhi::SamplerAddressMode::Repeat;
			case 33071:
				return nvrhi::SamplerAddressMode::ClampToEdge;
			case 33648:
				return nvrhi::SamplerAddressMode::MirroredRepeat;
			default:
				throw std::invalid_argument("Unsupported glTF texture address mode");
			}
		}
	} // namespace

	std::pair<glm::mat4, glm::mat4> CalculateSceneCamera(const Scene& scene, uint32_t width, uint32_t height)
	{
		scene.Validate();
		if (width == 0 || height == 0)
		{
			throw std::invalid_argument("Camera viewport dimensions must be positive");
		}
		Entity camera;
		for (const auto entity : scene.Entities())
		{
			const auto& data = scene.Get(entity);
			if (data.Camera && data.Camera->Primary)
			{
				if (camera)
				{
					throw std::invalid_argument("Rendering requires exactly one primary camera");
				}
				camera = entity;
			}
		}
		if (!camera)
		{
			throw std::invalid_argument("Rendering requires a primary camera");
		}
		const auto cameraWorld = scene.GetWorldTransform(camera);
		const auto cameraView = glm::inverse(cameraWorld);
		if (!IsFinite(cameraWorld) || !IsFinite(cameraView))
		{
			throw std::invalid_argument("Camera world transform must be finite and invertible");
		}
		const auto& cameraData = *scene.Get(camera).Camera;
		auto projection = glm::perspectiveRH_ZO(glm::radians(cameraData.VerticalFov),
												static_cast<float>(width) / static_cast<float>(height),
												cameraData.NearClip, cameraData.FarClip);
		return {cameraView, projection};
	}

	class SceneRenderPass::Impl
	{
	  public:
		explicit Impl(nvrhi::IDevice* device) : m_Device(device)
		{
			CreateSharedResources();
		}

		void Prepare(const Scene& scene, const AssetImporter& importer, nvrhi::ITexture* output,
					 const RenderSettings& settings)
		{
			scene.Validate();
			if (!std::isfinite(settings.Exposure) || settings.Exposure <= 0.0f || settings.Exposure > 64.0f ||
				!std::isfinite(settings.AmbientIntensity) || settings.AmbientIntensity < 0.0f ||
				settings.AmbientIntensity > 16.0f)
			{
				throw std::invalid_argument("Exposure must be in (0,64] and ambient intensity in [0,16]");
			}
			if (!std::isfinite(settings.EnvironmentIntensity) || settings.EnvironmentIntensity < 0.0f ||
				settings.EnvironmentIntensity > 64.0f || !std::isfinite(settings.EnvironmentRotation))
			{
				throw std::invalid_argument("Environment intensity must be finite and in [0,64]");
			}
			if (settings.ShadowResolution < 64 || settings.ShadowResolution > 2048 ||
				!std::isfinite(settings.ShadowBias) || settings.ShadowBias < 0 || settings.ShadowBias > 0.1f ||
				!std::isfinite(settings.ShadowSoftness) || settings.ShadowSoftness < 0 || settings.ShadowSoftness > 8)
			{
				throw std::invalid_argument(
					"Shadow resolution must be in [64,2048], bias in [0,0.1], softness in [0,8]");
			}
			if (!std::isfinite(settings.AmbientOcclusionRadius) || settings.AmbientOcclusionRadius < 0.001f ||
				settings.AmbientOcclusionRadius > 100.0f || !std::isfinite(settings.AmbientOcclusionBias) ||
				settings.AmbientOcclusionBias < 0.0f ||
				settings.AmbientOcclusionBias > settings.AmbientOcclusionRadius ||
				!std::isfinite(settings.AmbientOcclusionPower) || settings.AmbientOcclusionPower < 0.1f ||
				settings.AmbientOcclusionPower > 8.0f)
			{
				throw std::invalid_argument(
					"Ambient occlusion radius must be in [0.001,100], bias in [0,radius], power in [0.1,8]");
			}
			for (const auto channel : settings.BackgroundColor)
			{
				if (!std::isfinite(channel) || channel < 0.0f || channel > 65504.0f)
				{
					throw std::invalid_argument("HDR background channels must be finite and in [0,65504]");
				}
			}
			if (importer.GetRoot() != m_AssetRoot)
			{
				InvalidateAssets();
				m_AssetRoot = importer.GetRoot();
			}
			float environmentIntensity = m_HasEnvironment ? settings.EnvironmentIntensity : 0.0f;
			float environmentRotation = settings.EnvironmentRotation;
			if (settings.UseSceneEnvironment)
			{
				const auto& environment = scene.GetEnvironment();
				if (environment.Path.empty())
				{
					environmentIntensity = 0.0f;
				}
				else
				{
					const auto sourcePath = importer.ResolvePath(environment.Path);
					if (sourcePath != m_SceneEnvironmentPath)
					{
						const auto source = importer.LoadHDR(environment.Path);
						const auto fingerprint = EnvironmentProcessor::Fingerprint(source);
						const auto cache = std::filesystem::temp_directory_path() / "AsterEnvironment" /
										   (std::to_string(fingerprint) + ".asteribl");
						std::filesystem::create_directories(cache.parent_path());
						SetEnvironment(EnvironmentProcessor::BuildCached(source, cache));
						m_SceneEnvironmentPath = sourcePath;
					}
					environmentIntensity = environment.Intensity * settings.EnvironmentIntensity;
					environmentRotation += environment.Rotation;
				}
			}
			if (!std::isfinite(environmentIntensity) || !std::isfinite(environmentRotation))
			{
				throw std::invalid_argument("Combined environment intensity or rotation is nonfinite");
			}
			EnsureTargets(output);
			m_Settings = settings;
			m_Frame = {};
			m_Draws.clear();
			const auto [cameraView, projection] =
				settings.CameraOverride
					? std::pair{settings.CameraOverride->View, settings.CameraOverride->Projection}
					: CalculateSceneCamera(scene, output->getDesc().width, output->getDesc().height);
			const auto cameraWorld = glm::inverse(cameraView);
			if (!IsFinite(cameraView) || !IsFinite(cameraWorld) || !IsFinite(projection) ||
				!IsFinite(glm::inverse(projection)))
			{
				throw std::invalid_argument("Camera override matrices must be finite and invertible");
			}
			m_Frame.ViewProjection = projection * cameraView;
			if (!IsFinite(m_Frame.ViewProjection))
			{
				throw std::invalid_argument("Combined camera view-projection matrix is nonfinite");
			}
			m_Occlusion.Projection = projection;
			m_Occlusion.InverseProjection = glm::inverse(projection);
			m_Occlusion.Parameters = glm::vec4(settings.AmbientOcclusionRadius, settings.AmbientOcclusionBias,
											   settings.AmbientOcclusionPower, 0);
			m_Occlusion.Size = glm::vec4(1.0f / output->getDesc().width, 1.0f / output->getDesc().height, 0, 0);
			m_Frame.CameraPosition = glm::vec4(glm::vec3(cameraWorld[3]), 1.0f);
			m_Frame.Parameters = glm::vec4(0.0f, settings.Exposure, settings.AmbientIntensity, environmentIntensity);
			m_Frame.Environment = glm::vec4(std::cos(environmentRotation), std::sin(environmentRotation), 0, 0);
			m_Frame.Shadow = glm::vec4(settings.ShadowBias, settings.ShadowSoftness, 0, 0);
			uint32_t lightCount = 0;
			for (const auto entity : scene.Entities())
			{
				const auto& data = scene.Get(entity);
				const auto world = scene.GetWorldTransform(entity);
				if (!IsFinite(world) || !IsFinite(glm::inverse(world)))
				{
					throw std::invalid_argument("Rendered entity world transforms must be finite and invertible");
				}
				if (data.Light)
				{
					if (lightCount == m_Frame.Lights.size())
					{
						throw std::invalid_argument("The forward renderer supports at most 32 active lights");
					}
					const auto& light = *data.Light;
					auto& constant = m_Frame.Lights[lightCount++];
					constant.PositionType = glm::vec4(glm::vec3(world[3]), static_cast<float>(light.Type));
					const glm::vec3 direction = glm::mat3(world) * glm::vec3(0, 0, -1);
					const float directionScale =
						std::max({std::abs(direction.x), std::abs(direction.y), std::abs(direction.z)});
					if (!std::isfinite(directionScale) || directionScale <= 0.0f)
					{
						throw std::invalid_argument("Light world direction must be finite and nonzero");
					}
					// Scale before normalization so extreme, valid hierarchy scales do not overflow/underflow its
					// squared length.
					const auto normalizedDirection = glm::normalize(direction / directionScale);
					constant.DirectionRange = glm::vec4(normalizedDirection, light.Range);
					constant.ColorIntensity = glm::vec4(light.Color, light.Intensity);
					constant.Cone =
						glm::vec4(std::cos(glm::radians(light.InnerCone)), std::cos(glm::radians(light.OuterCone)), -1,
								  light.CastShadows && settings.Shadows ? 1.0f : 0.0f);
				}
				if (!data.MeshRenderer || !data.MeshRenderer->Visible)
				{
					continue;
				}
				const auto& component = *data.MeshRenderer;
				auto& mesh = GetMesh(importer, component.Mesh);
				for (const auto& primitive : mesh.Primitives)
				{
					DrawItem item;
					item.Primitive = &primitive;
					item.Material = &mesh.Materials[primitive.MaterialIndex];
					item.Constants.Model = world * primitive.Transform;
					item.Constants.Color = component.BaseColor;
					item.Constants.Material = glm::vec4(component.Metallic, component.Roughness, 0, 0);
					if (!IsFinite(item.Constants.Model) || !IsFinite(glm::inverse(item.Constants.Model)))
					{
						throw std::invalid_argument(
							"Combined entity and glTF node transform must be finite and invertible");
					}
					item.PipelineIndex = (item.Material->Blend ? 1u : 0u) | (item.Material->DoubleSided ? 2u : 0u) |
										 (glm::determinant(glm::mat3(item.Constants.Model)) < 0.0f ? 4u : 0u);
					const auto viewCenter = cameraView * item.Constants.Model * glm::vec4(primitive.Center, 1.0f);
					item.Depth = -viewCenter.z;
					m_Draws.push_back(item);
				}
			}
			m_Frame.Parameters.x = static_cast<float>(lightCount);
			std::stable_sort(m_Draws.begin(), m_Draws.end(),
							 [](const auto& first, const auto& second)
							 {
								 if (first.Material->Blend != second.Material->Blend)
								 {
									 return !first.Material->Blend;
								 }
								 return first.Material->Blend && first.Depth > second.Depth;
							 });
			BuildShadowViews();
			EnsureShadowTarget(settings.ShadowResolution, std::max(1u, static_cast<uint32_t>(m_ShadowFaces.size())));
		}

		void Record(nvrhi::ICommandList* commands)
		{
			commands->writeBuffer(m_FrameBuffer, &m_Frame, sizeof(m_Frame));
			RecordShadows(commands);
			RecordDepthAndOcclusion(commands);
			commands->clearTextureFloat(m_Hdr, nvrhi::AllSubresources,
										nvrhi::Color(m_Settings.BackgroundColor[0], m_Settings.BackgroundColor[1],
													 m_Settings.BackgroundColor[2], 1.0f));
			const auto& target = m_Hdr->getDesc();
			const auto viewport = nvrhi::ViewportState().addViewportAndScissorRect(
				nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
			if (m_HasEnvironment && m_Settings.DrawSky && m_Frame.Parameters.w > 0.0f)
			{
				nvrhi::GraphicsState sky;
				sky.pipeline = m_SkyPipeline;
				sky.framebuffer = m_HdrFramebuffer;
				sky.viewport = viewport;
				sky.bindings = {m_EnvironmentBindings};
				commands->setGraphicsState(sky);
				commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			}
			for (const auto& item : m_Draws)
			{
				nvrhi::GraphicsState state;
				state.pipeline = m_PbrPipelines[item.PipelineIndex];
				state.framebuffer = m_HdrFramebuffer;
				state.viewport = viewport;
				state.bindings = {item.Material->Bindings, m_EnvironmentBindings};
				state.vertexBuffers = {{item.Primitive->Vertices, 0, 0}};
				state.indexBuffer = {item.Primitive->Indices, nvrhi::Format::R32_UINT, 0};
				commands->setGraphicsState(state);
				commands->setPushConstants(&item.Constants, sizeof(item.Constants));
				commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(item.Primitive->IndexCount));
			}
			nvrhi::GraphicsState tone;
			tone.pipeline = m_TonePipeline;
			tone.framebuffer = m_OutputFramebuffer;
			tone.viewport = viewport;
			tone.bindings = {m_ToneBindings};
			commands->setGraphicsState(tone);
			const glm::vec4 toneConstants(m_Settings.Exposure, 0, 0, 0);
			commands->setPushConstants(&toneConstants, sizeof(toneConstants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		void InvalidateAssets()
		{
			m_Draws.clear();
			m_Meshes.clear();
			m_SceneEnvironmentPath.clear();
		}

		void SetEnvironment(const EnvironmentMaps& environment)
		{
			const auto validate = [](const HDRImageAsset& image)
			{
				if (image.Width == 0 || image.Height == 0 || image.Width > 16384 || image.Height > 16384 ||
					image.Pixels.size() != static_cast<size_t>(image.Width) * image.Height * 4)
				{
					throw std::invalid_argument("Environment image dimensions and pixel count are inconsistent");
				}
				for (const float value : image.Pixels)
				{
					if (!std::isfinite(value) || value < 0.0f)
					{
						throw std::invalid_argument("Environment images require finite, nonnegative radiance");
					}
				}
			};
			validate(environment.Sky);
			validate(environment.Irradiance);
			validate(environment.BRDFLut);
			if (environment.SpecularMips.empty() || environment.SpecularMips.size() > 15)
			{
				throw std::invalid_argument("Environment requires one to fifteen specular mip levels");
			}
			uint32_t width = environment.SpecularMips.front().Width;
			uint32_t height = environment.SpecularMips.front().Height;
			uint32_t maximumMipCount = 1;
			for (uint32_t extent = std::max(width, height); extent > 1; extent /= 2)
			{
				++maximumMipCount;
			}
			if (environment.SpecularMips.size() > maximumMipCount)
			{
				throw std::invalid_argument("Environment specular mip count exceeds its texture dimensions");
			}
			for (const auto& mip : environment.SpecularMips)
			{
				validate(mip);
				if (mip.Width != width || mip.Height != height)
				{
					throw std::invalid_argument("Environment specular mip dimensions must halve at each level");
				}
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
			}
			const std::array<const HDRImageAsset*, 4> baseImages{
				&environment.Irradiance, &environment.SpecularMips.front(), &environment.BRDFLut, &environment.Sky};
			std::array<nvrhi::TextureHandle, 4> textures;
			for (size_t index = 0; index < textures.size(); ++index)
			{
				nvrhi::TextureDesc texture;
				texture.width = baseImages[index]->Width;
				texture.height = baseImages[index]->Height;
				texture.mipLevels = index == 1 ? static_cast<uint32_t>(environment.SpecularMips.size()) : 1;
				texture.format = nvrhi::Format::RGBA32_FLOAT;
				texture.debugName = "Aster environment lighting";
				texture.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
				textures[index] = m_Device->createTexture(texture);
				RequireResource(textures[index] != nullptr, "environment lighting texture");
			}
			m_UploadCommands->open();
			for (size_t index = 0; index < textures.size(); ++index)
			{
				const auto& image = *baseImages[index];
				m_UploadCommands->writeTexture(textures[index], 0, 0, image.Pixels.data(),
											   static_cast<size_t>(image.Width) * sizeof(float) * 4);
			}
			for (uint32_t mip = 1; mip < environment.SpecularMips.size(); ++mip)
			{
				const auto& image = environment.SpecularMips[mip];
				m_UploadCommands->writeTexture(textures[1], 0, mip, image.Pixels.data(),
											   static_cast<size_t>(image.Width) * sizeof(float) * 4);
			}
			SubmitUploads();
			auto bindingSet = CreateEnvironmentBindings(textures, m_ShadowTexture);
			m_EnvironmentTextures = std::move(textures);
			m_EnvironmentBindings = std::move(bindingSet);
			m_HasEnvironment = true;
			m_SceneEnvironmentPath.clear();
		}

	  private:
		struct GpuPrimitive
		{
			nvrhi::BufferHandle Vertices;
			nvrhi::BufferHandle Indices;
			uint32_t IndexCount = 0;
			uint32_t MaterialIndex = 0;
			glm::mat4 Transform{1.0f};
			glm::vec3 Center{0.0f};
			glm::vec3 Minimum{0.0f};
			glm::vec3 Maximum{0.0f};
		};

		struct GpuMaterial
		{
			nvrhi::BufferHandle Constants;
			nvrhi::BindingSetHandle Bindings;
			bool Blend = false;
			bool DoubleSided = false;
		};

		struct GpuTexture
		{
			nvrhi::TextureHandle Texture;
			nvrhi::TextureHandle ColorTexture;
			nvrhi::SamplerHandle Sampler;
		};

		struct GpuMesh
		{
			std::vector<GpuPrimitive> Primitives;
			std::vector<GpuMaterial> Materials;
			std::vector<GpuTexture> Textures;
		};

		struct DrawItem
		{
			const GpuPrimitive* Primitive = nullptr;
			const GpuMaterial* Material = nullptr;
			DrawConstants Constants;
			uint32_t PipelineIndex = 0;
			float Depth = 0.0f;
		};

		struct ShadowFace
		{
			uint32_t Light;
			uint32_t Face;
		};

		nvrhi::ShaderHandle CreateShader(nvrhi::ShaderType type, const uint32_t* bytes, size_t size, const char* name)
		{
			nvrhi::ShaderDesc description;
			description.shaderType = type;
			description.debugName = name;
			auto shader = m_Device->createShader(description, bytes, size);
			RequireResource(shader != nullptr, name);
			return shader;
		}

		void CreateSharedResources()
		{
			m_PbrVertex = CreateShader(nvrhi::ShaderType::Vertex, Shaders::PbrVert, sizeof(Shaders::PbrVert),
									   "PBR vertex shader");
			m_PbrPixel =
				CreateShader(nvrhi::ShaderType::Pixel, Shaders::PbrFrag, sizeof(Shaders::PbrFrag), "PBR pixel shader");
			m_ToneVertex = CreateShader(nvrhi::ShaderType::Vertex, Shaders::FullscreenVert,
										sizeof(Shaders::FullscreenVert), "Fullscreen vertex shader");
			m_TonePixel = CreateShader(nvrhi::ShaderType::Pixel, Shaders::TonemapFrag, sizeof(Shaders::TonemapFrag),
									   "Tonemap pixel shader");
			m_SkyPixel =
				CreateShader(nvrhi::ShaderType::Pixel, Shaders::SkyFrag, sizeof(Shaders::SkyFrag), "Sky pixel shader");
			m_ShadowVertex = CreateShader(nvrhi::ShaderType::Vertex, Shaders::ShadowVert, sizeof(Shaders::ShadowVert),
										  "Shadow vertex shader");
			m_DepthPixel = CreateShader(nvrhi::ShaderType::Pixel, Shaders::DepthFrag, sizeof(Shaders::DepthFrag),
										"Depth alpha mask shader");
			m_DepthVertex = CreateShader(nvrhi::ShaderType::Vertex, Shaders::DepthVert, sizeof(Shaders::DepthVert),
										 "Depth prepass vertex shader");
			m_OcclusionPixel = CreateShader(nvrhi::ShaderType::Pixel, Shaders::SsaoFrag, sizeof(Shaders::SsaoFrag),
											"SSAO pixel shader");
			m_OcclusionBlurPixel = CreateShader(nvrhi::ShaderType::Pixel, Shaders::SsaoBlurFrag,
												sizeof(Shaders::SsaoBlurFrag), "SSAO bilateral blur shader");
			std::array<nvrhi::VertexAttributeDesc, 6> attributes;
			attributes[0]
				.setName("POSITION")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(MeshVertex, Position));
			attributes[1]
				.setName("NORMAL")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(MeshVertex, Normal));
			attributes[2]
				.setName("TANGENT")
				.setFormat(nvrhi::Format::RGBA32_FLOAT)
				.setOffset(offsetof(MeshVertex, Tangent));
			attributes[3]
				.setName("TEXCOORD0")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(MeshVertex, TexCoord));
			attributes[4]
				.setName("TEXCOORD1")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(MeshVertex, TexCoord1));
			attributes[5]
				.setName("COLOR")
				.setFormat(nvrhi::Format::RGBA32_FLOAT)
				.setOffset(offsetof(MeshVertex, Color));
			for (auto& attribute : attributes)
			{
				attribute.elementStride = sizeof(MeshVertex);
			}
			m_VertexLayout =
				m_Device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), m_PbrVertex);
			RequireResource(m_VertexLayout != nullptr, "mesh vertex layout");
			const std::array depthAttributes{attributes[0], attributes[3], attributes[4], attributes[5]};
			m_DepthVertexLayout = m_Device->createInputLayout(
				depthAttributes.data(), static_cast<uint32_t>(depthAttributes.size()), m_ShadowVertex);
			RequireResource(m_DepthVertexLayout != nullptr, "depth vertex layout");
			nvrhi::BindingLayoutDesc pbrLayout;
			pbrLayout.visibility = nvrhi::ShaderType::AllGraphics;
			pbrLayout.setRegisterSpaceAndDescriptorSet(0);
			pbrLayout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
								  nvrhi::BindingLayoutItem::ConstantBuffer(1),
								  nvrhi::BindingLayoutItem::PushConstants(2, sizeof(DrawConstants))};
			for (uint32_t index = 0; index < 5; ++index)
			{
				pbrLayout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(index));
				pbrLayout.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(index));
			}
			m_PbrLayout = m_Device->createBindingLayout(pbrLayout);
			RequireResource(m_PbrLayout != nullptr, "PBR binding layout");
			nvrhi::BindingLayoutDesc environmentLayout;
			environmentLayout.visibility = nvrhi::ShaderType::AllGraphics;
			environmentLayout.setRegisterSpaceAndDescriptorSet(1);
			environmentLayout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
										  nvrhi::BindingLayoutItem::Sampler(0),
										  nvrhi::BindingLayoutItem::Sampler(1),
										  nvrhi::BindingLayoutItem::Sampler(2),
										  nvrhi::BindingLayoutItem::Texture_SRV(4),
										  nvrhi::BindingLayoutItem::Texture_SRV(5)};
			for (uint32_t index = 0; index < 4; ++index)
			{
				environmentLayout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(index));
			}
			m_EnvironmentLayout = m_Device->createBindingLayout(environmentLayout);
			RequireResource(m_EnvironmentLayout != nullptr, "environment lighting layout");
			nvrhi::BindingLayoutDesc toneLayout;
			toneLayout.visibility = nvrhi::ShaderType::AllGraphics;
			toneLayout.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0),
								   nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec4))};
			m_ToneLayout = m_Device->createBindingLayout(toneLayout);
			RequireResource(m_ToneLayout != nullptr, "tonemap binding layout");
			nvrhi::BindingLayoutDesc occlusionLayout;
			occlusionLayout.visibility = nvrhi::ShaderType::AllGraphics;
			occlusionLayout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
										nvrhi::BindingLayoutItem::Texture_SRV(0),
										nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0)};
			m_OcclusionLayout = m_Device->createBindingLayout(occlusionLayout);
			RequireResource(m_OcclusionLayout != nullptr, "SSAO binding layout");
			nvrhi::BufferDesc frameBuffer;
			frameBuffer.byteSize = sizeof(FrameConstants);
			frameBuffer.isConstantBuffer = true;
			frameBuffer.isVolatile = true;
			frameBuffer.maxVersions = 16;
			frameBuffer.debugName = "Aster frame constants";
			m_FrameBuffer = m_Device->createBuffer(frameBuffer);
			RequireResource(m_FrameBuffer != nullptr, "frame constant buffer");
			frameBuffer.byteSize = sizeof(OcclusionConstants);
			frameBuffer.debugName = "Aster SSAO constants";
			m_OcclusionBuffer = m_Device->createBuffer(frameBuffer);
			RequireResource(m_OcclusionBuffer != nullptr, "SSAO constant buffer");
			m_LinearSampler = m_Device->createSampler(nvrhi::SamplerDesc());
			RequireResource(m_LinearSampler != nullptr, "linear sampler");
			m_EnvironmentSampler =
				m_Device->createSampler(nvrhi::SamplerDesc().setAddressU(nvrhi::SamplerAddressMode::Repeat));
			RequireResource(m_EnvironmentSampler != nullptr, "environment lighting sampler");
			m_ShadowSampler = m_Device->createSampler(nvrhi::SamplerDesc().setAllFilters(false));
			RequireResource(m_ShadowSampler != nullptr, "shadow depth sampler");
			EnsureShadowTarget(512, 1);
			m_UploadCommands = m_Device->createCommandList();
			RequireResource(m_UploadCommands != nullptr, "asset upload command list");
			nvrhi::TextureDesc white;
			white.format = nvrhi::Format::RGBA8_UNORM;
			white.isTypeless = true;
			white.debugName = "Aster default white texture";
			white.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
			m_WhiteTexture = m_Device->createTexture(white);
			RequireResource(m_WhiteTexture != nullptr, "default white texture");
			const std::array<uint8_t, 4> pixel{255, 255, 255, 255};
			m_UploadCommands->open();
			m_UploadCommands->writeTexture(m_WhiteTexture, 0, 0, pixel.data(), 4);
			SubmitUploads();
			EnvironmentMaps darkEnvironment;
			darkEnvironment.Sky = {1, 1, {0, 0, 0, 1}};
			darkEnvironment.Irradiance = darkEnvironment.Sky;
			darkEnvironment.SpecularMips = {darkEnvironment.Sky};
			darkEnvironment.BRDFLut = darkEnvironment.Sky;
			SetEnvironment(darkEnvironment);
			m_HasEnvironment = false;
		}

		nvrhi::BindingSetHandle CreateEnvironmentBindings(const std::array<nvrhi::TextureHandle, 4>& textures,
														  nvrhi::ITexture* shadows)
		{
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameBuffer),
								 nvrhi::BindingSetItem::Sampler(0, m_EnvironmentSampler),
								 nvrhi::BindingSetItem::Sampler(1, m_LinearSampler),
								 nvrhi::BindingSetItem::Sampler(2, m_ShadowSampler),
								 nvrhi::BindingSetItem::Texture_SRV(4, shadows),
								 nvrhi::BindingSetItem::Texture_SRV(5, m_OcclusionFiltered ? m_OcclusionFiltered.Get()
																						   : m_WhiteTexture.Get())};
			for (uint32_t index = 0; index < textures.size(); ++index)
			{
				bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(index, textures[index]));
			}
			auto result = m_Device->createBindingSet(bindings, m_EnvironmentLayout);
			RequireResource(result != nullptr, "environment and shadow bindings");
			return result;
		}

		void EnsureShadowTarget(uint32_t resolution, uint32_t layerCount)
		{
			if (m_ShadowTexture && m_ShadowTexture->getDesc().width == resolution &&
				m_ShadowTexture->getDesc().arraySize == layerCount)
			{
				return;
			}
			if (static_cast<uint64_t>(resolution) * resolution * layerCount * 4 > 512ull * 1024 * 1024)
			{
				throw std::invalid_argument(
					"Shadow maps exceed the 512 MiB budget; reduce resolution or shadow-casting lights");
			}
			nvrhi::TextureDesc description;
			description.width = resolution;
			description.height = resolution;
			description.arraySize = layerCount;
			description.dimension = nvrhi::TextureDimension::Texture2DArray;
			description.format = nvrhi::Format::D32;
			description.isRenderTarget = true;
			description.debugName = "Aster shadow depth array";
			description.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
			auto texture = m_Device->createTexture(description);
			RequireResource(texture != nullptr, "shadow depth array");
			std::vector<nvrhi::FramebufferHandle> framebuffers;
			for (uint32_t layer = 0; layer < layerCount; ++layer)
			{
				auto framebuffer = m_Device->createFramebuffer(
					nvrhi::FramebufferDesc().setDepthAttachment(texture, nvrhi::TextureSubresourceSet(0, 1, layer, 1)));
				RequireResource(framebuffer != nullptr, "shadow layer framebuffer");
				framebuffers.push_back(std::move(framebuffer));
			}
			std::array<nvrhi::GraphicsPipelineHandle, 4> pipelines;
			for (uint32_t index = 0; index < pipelines.size(); ++index)
			{
				nvrhi::GraphicsPipelineDesc pipeline;
				pipeline.VS = m_ShadowVertex;
				pipeline.PS = m_DepthPixel;
				pipeline.inputLayout = m_DepthVertexLayout;
				pipeline.bindingLayouts = {m_PbrLayout};
				pipeline.renderState.rasterState.cullMode =
					(index & 1) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
				pipeline.renderState.rasterState.frontCounterClockwise = (index & 2) == 0;
				pipeline.renderState.rasterState.depthBias = 1;
				pipeline.renderState.rasterState.slopeScaledDepthBias = 1.0f;
				pipeline.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
				pipelines[index] =
					m_Device->createGraphicsPipeline(pipeline, framebuffers.front()->getFramebufferInfo());
				RequireResource(pipelines[index] != nullptr, "shadow graphics pipeline");
			}
			nvrhi::BindingSetHandle bindings;
			if (m_EnvironmentTextures[0])
			{
				bindings = CreateEnvironmentBindings(m_EnvironmentTextures, texture);
			}
			m_ShadowFramebuffers = std::move(framebuffers);
			m_ShadowTexture = std::move(texture);
			m_ShadowPipelines = std::move(pipelines);
			if (bindings)
			{
				m_EnvironmentBindings = std::move(bindings);
			}
		}

		void BuildShadowViews()
		{
			m_ShadowFaces.clear();
			if (!m_Settings.Shadows || m_Draws.empty())
			{
				return;
			}
			glm::vec3 minimum(std::numeric_limits<float>::max());
			glm::vec3 maximum(std::numeric_limits<float>::lowest());
			for (const auto& item : m_Draws)
			{
				for (uint32_t corner = 0; corner < 8; ++corner)
				{
					glm::vec3 position;
					for (uint32_t axis = 0; axis < 3; ++axis)
					{
						position[axis] =
							(corner & (1u << axis)) ? item.Primitive->Maximum[axis] : item.Primitive->Minimum[axis];
					}
					position = glm::vec3(item.Constants.Model * glm::vec4(position, 1));
					minimum = glm::min(minimum, position);
					maximum = glm::max(maximum, position);
				}
			}
			const glm::vec3 center = minimum * 0.5f + maximum * 0.5f;
			const float radius = std::max(glm::length(maximum * 0.5f - minimum * 0.5f), 0.01f);
			if (!std::isfinite(radius))
			{
				throw std::invalid_argument("Shadow scene bounds overflowed");
			}
			const std::array<glm::vec3, 6> directions{
				{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
			const std::array<glm::vec3, 6> upDirections{
				{{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}}};
			for (uint32_t index = 0; index < static_cast<uint32_t>(m_Frame.Parameters.x); ++index)
			{
				auto& light = m_Frame.Lights[index];
				if (light.Cone.w == 0 || light.ColorIntensity.w == 0)
				{
					continue;
				}
				const auto type = static_cast<LightType>(static_cast<int>(light.PositionType.w));
				const glm::vec3 position(light.PositionType);
				const glm::vec3 direction(light.DirectionRange);
				const glm::vec3 up = std::abs(direction.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
				uint32_t faceCount = 1;
				if (type == LightType::Directional)
				{
					const auto view = glm::lookAtRH(center - direction * (radius * 2), center, up);
					// A sphere fit is stable under object/light rotation and includes every scene bound.
					const float extent = radius * 1.02f;
					light.ShadowMatrices[0] =
						glm::orthoRH_ZO(-extent, extent, -extent, extent, radius * 0.5f, radius * 3.5f) * view;
				}
				else
				{
					const float farPlane = light.DirectionRange.w;
					const float nearPlane = farPlane * 0.0001f;
					if (nearPlane <= 0.0f)
					{
						throw std::invalid_argument("Shadow light range is below floating-point precision");
					}
					if (type == LightType::Point)
					{
						faceCount = 6;
						const auto projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
						for (uint32_t face = 0; face < faceCount; ++face)
						{
							light.ShadowMatrices[face] =
								projection * glm::lookAtRH(position, position + directions[face], upDirections[face]);
						}
					}
					else
					{
						const float fieldOfView = 2.0f * std::acos(glm::clamp(light.Cone.y, -1.0f, 1.0f));
						if (fieldOfView <= 0.0f)
						{
							continue;
						}
						light.ShadowMatrices[0] = glm::perspectiveRH_ZO(fieldOfView, 1.0f, nearPlane, farPlane) *
												  glm::lookAtRH(position, position + direction, up);
					}
				}
				light.Cone.z = static_cast<float>(m_ShadowFaces.size());
				for (uint32_t face = 0; face < faceCount; ++face)
				{
					if (!IsFinite(light.ShadowMatrices[face]))
					{
						throw std::invalid_argument("Shadow view is nonfinite");
					}
					m_ShadowFaces.push_back({index, face});
				}
			}
		}

		void RecordShadows(nvrhi::ICommandList* commands)
		{
			commands->clearDepthStencilTexture(m_ShadowTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);
			const float resolution = static_cast<float>(m_ShadowTexture->getDesc().width);
			const auto viewport =
				nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(resolution, resolution));
			for (size_t layer = 0; layer < m_ShadowFaces.size(); ++layer)
			{
				for (const auto& item : m_Draws)
				{
					// Translucent surfaces need transmission shadows; only opaque/masked surfaces cast here.
					if (item.Material->Blend)
					{
						continue;
					}
					nvrhi::GraphicsState state;
					state.pipeline = m_ShadowPipelines[item.PipelineIndex >> 1];
					state.framebuffer = m_ShadowFramebuffers[layer];
					state.viewport = viewport;
					state.bindings = {item.Material->Bindings};
					state.vertexBuffers = {{item.Primitive->Vertices, 0, 0}};
					state.indexBuffer = {item.Primitive->Indices, nvrhi::Format::R32_UINT, 0};
					commands->setGraphicsState(state);
					auto constants = item.Constants;
					constants.Material.z = static_cast<float>(m_ShadowFaces[layer].Light);
					constants.Material.w = static_cast<float>(m_ShadowFaces[layer].Face);
					commands->setPushConstants(&constants, sizeof(constants));
					commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(item.Primitive->IndexCount));
				}
			}
		}

		void SubmitUploads()
		{
			m_UploadCommands->close();
			m_Device->executeCommandList(m_UploadCommands);
			if (!m_Device->waitForIdle())
			{
				throw std::runtime_error("Waiting for mesh/texture uploads failed");
			}
			m_Device->runGarbageCollection();
		}

		void CreateOcclusionTargets()
		{
			auto texture = m_Hdr->getDesc();
			texture.format = nvrhi::Format::R8_UNORM;
			texture.debugName = "Aster SSAO raw";
			m_OcclusionRaw = m_Device->createTexture(texture);
			RequireResource(m_OcclusionRaw != nullptr, "SSAO raw target");
			texture.debugName = "Aster SSAO bilateral filtered";
			m_OcclusionFiltered = m_Device->createTexture(texture);
			RequireResource(m_OcclusionFiltered != nullptr, "SSAO filtered target");
			m_DepthFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(m_Depth));
			RequireResource(m_DepthFramebuffer != nullptr, "scene depth prepass framebuffer");
			m_OcclusionFramebuffer =
				m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OcclusionRaw));
			RequireResource(m_OcclusionFramebuffer != nullptr, "SSAO framebuffer");
			m_OcclusionBlurFramebuffer =
				m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OcclusionFiltered));
			RequireResource(m_OcclusionBlurFramebuffer != nullptr, "SSAO blur framebuffer");
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_OcclusionBuffer),
								 nvrhi::BindingSetItem::Texture_SRV(0, m_Depth),
								 nvrhi::BindingSetItem::Texture_SRV(1, m_WhiteTexture),
								 nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler)};
			m_OcclusionBindings = m_Device->createBindingSet(bindings, m_OcclusionLayout);
			RequireResource(m_OcclusionBindings != nullptr, "SSAO depth bindings");
			bindings.bindings[2] = nvrhi::BindingSetItem::Texture_SRV(1, m_OcclusionRaw);
			m_OcclusionBlurBindings = m_Device->createBindingSet(bindings, m_OcclusionLayout);
			RequireResource(m_OcclusionBlurBindings != nullptr, "SSAO bilateral blur bindings");
			for (uint32_t index = 0; index < m_DepthPipelines.size(); ++index)
			{
				nvrhi::GraphicsPipelineDesc pipeline;
				pipeline.VS = m_DepthVertex;
				pipeline.PS = m_DepthPixel;
				pipeline.inputLayout = m_DepthVertexLayout;
				pipeline.bindingLayouts = {m_PbrLayout};
				pipeline.renderState.rasterState.cullMode =
					(index & 1) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
				pipeline.renderState.rasterState.frontCounterClockwise = (index & 2) == 0;
				pipeline.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
				m_DepthPipelines[index] =
					m_Device->createGraphicsPipeline(pipeline, m_DepthFramebuffer->getFramebufferInfo());
				RequireResource(m_DepthPipelines[index] != nullptr, "scene depth prepass pipeline");
			}
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = m_ToneVertex;
			pipeline.PS = m_OcclusionPixel;
			pipeline.bindingLayouts = {m_OcclusionLayout};
			pipeline.renderState.depthStencilState.depthTestEnable = false;
			pipeline.renderState.depthStencilState.depthWriteEnable = false;
			pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			m_OcclusionPipeline =
				m_Device->createGraphicsPipeline(pipeline, m_OcclusionFramebuffer->getFramebufferInfo());
			RequireResource(m_OcclusionPipeline != nullptr, "SSAO sampling pipeline");
			pipeline.PS = m_OcclusionBlurPixel;
			m_OcclusionBlurPipeline =
				m_Device->createGraphicsPipeline(pipeline, m_OcclusionBlurFramebuffer->getFramebufferInfo());
			RequireResource(m_OcclusionBlurPipeline != nullptr, "SSAO bilateral blur pipeline");
			m_EnvironmentBindings = CreateEnvironmentBindings(m_EnvironmentTextures, m_ShadowTexture);
		}

		void RecordDepthAndOcclusion(nvrhi::ICommandList* commands)
		{
			commands->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, 1.0f, false, 0);
			const auto& target = m_Depth->getDesc();
			const auto viewport = nvrhi::ViewportState().addViewportAndScissorRect(
				nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
			for (const auto& item : m_Draws)
			{
				if (item.Material->Blend)
				{
					continue;
				}
				nvrhi::GraphicsState state;
				state.pipeline = m_DepthPipelines[item.PipelineIndex >> 1];
				state.framebuffer = m_DepthFramebuffer;
				state.viewport = viewport;
				state.bindings = {item.Material->Bindings};
				state.vertexBuffers = {{item.Primitive->Vertices, 0, 0}};
				state.indexBuffer = {item.Primitive->Indices, nvrhi::Format::R32_UINT, 0};
				commands->setGraphicsState(state);
				commands->setPushConstants(&item.Constants, sizeof(item.Constants));
				commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(item.Primitive->IndexCount));
			}
			if (!m_Settings.AmbientOcclusion || m_Draws.empty())
			{
				commands->clearTextureFloat(m_OcclusionFiltered, nvrhi::AllSubresources, nvrhi::Color(1.0f));
				return;
			}
			commands->writeBuffer(m_OcclusionBuffer, &m_Occlusion, sizeof(m_Occlusion));
			nvrhi::GraphicsState occlusion;
			occlusion.pipeline = m_OcclusionPipeline;
			occlusion.framebuffer = m_OcclusionFramebuffer;
			occlusion.viewport = viewport;
			occlusion.bindings = {m_OcclusionBindings};
			commands->setGraphicsState(occlusion);
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			occlusion.pipeline = m_OcclusionBlurPipeline;
			occlusion.framebuffer = m_OcclusionBlurFramebuffer;
			occlusion.bindings = {m_OcclusionBlurBindings};
			commands->setGraphicsState(occlusion);
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		void EnsureTargets(nvrhi::ITexture* output)
		{
			if (m_Output == output)
			{
				return;
			}
			nvrhi::TextureDesc hdr;
			hdr.width = output->getDesc().width;
			hdr.height = output->getDesc().height;
			hdr.format = nvrhi::Format::RGBA16_FLOAT;
			hdr.isRenderTarget = true;
			hdr.debugName = "Aster HDR scene color";
			hdr.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
			m_Hdr = m_Device->createTexture(hdr);
			RequireResource(m_Hdr != nullptr, "HDR render target");
			auto depth = hdr;
			depth.format = nvrhi::Format::D32;
			depth.debugName = "Aster scene depth";
			depth.enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);
			m_Depth = m_Device->createTexture(depth);
			RequireResource(m_Depth != nullptr, "scene depth texture");
			m_HdrFramebuffer = m_Device->createFramebuffer(
				nvrhi::FramebufferDesc().addColorAttachment(m_Hdr).setDepthAttachment(m_Depth));
			RequireResource(m_HdrFramebuffer != nullptr, "HDR framebuffer");
			m_OutputFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
			RequireResource(m_OutputFramebuffer != nullptr, "tonemap output framebuffer");
			nvrhi::BindingSetDesc toneBindings;
			toneBindings.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, m_Hdr),
									 nvrhi::BindingSetItem::Sampler(0, m_LinearSampler),
									 nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec4))};
			m_ToneBindings = m_Device->createBindingSet(toneBindings, m_ToneLayout);
			RequireResource(m_ToneBindings != nullptr, "tonemap bindings");
			for (uint32_t index = 0; index < m_PbrPipelines.size(); ++index)
			{
				nvrhi::GraphicsPipelineDesc pipeline;
				pipeline.VS = m_PbrVertex;
				pipeline.PS = m_PbrPixel;
				pipeline.inputLayout = m_VertexLayout;
				pipeline.bindingLayouts = {m_PbrLayout, m_EnvironmentLayout};
				pipeline.renderState.rasterState.cullMode =
					(index & 2) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
				pipeline.renderState.rasterState.frontCounterClockwise = (index & 4) == 0;
				pipeline.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
				pipeline.renderState.depthStencilState.depthWriteEnable = false;
				if ((index & 1) != 0)
				{
					pipeline.renderState.depthStencilState.depthWriteEnable = false;
					auto& blend = pipeline.renderState.blendState.targets[0];
					blend.blendEnable = true;
					blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
					blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
					blend.srcBlendAlpha = nvrhi::BlendFactor::One;
					blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
				}
				m_PbrPipelines[index] =
					m_Device->createGraphicsPipeline(pipeline, m_HdrFramebuffer->getFramebufferInfo());
				RequireResource(m_PbrPipelines[index] != nullptr, "PBR graphics pipeline");
			}
			nvrhi::GraphicsPipelineDesc tone;
			tone.VS = m_ToneVertex;
			tone.PS = m_TonePixel;
			tone.bindingLayouts = {m_ToneLayout};
			tone.renderState.depthStencilState.depthTestEnable = false;
			tone.renderState.depthStencilState.depthWriteEnable = false;
			tone.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			m_TonePipeline = m_Device->createGraphicsPipeline(tone, m_OutputFramebuffer->getFramebufferInfo());
			RequireResource(m_TonePipeline != nullptr, "tonemap graphics pipeline");
			nvrhi::GraphicsPipelineDesc sky = tone;
			sky.PS = m_SkyPixel;
			sky.bindingLayouts = {m_EnvironmentLayout};
			m_SkyPipeline = m_Device->createGraphicsPipeline(sky, m_HdrFramebuffer->getFramebufferInfo());
			RequireResource(m_SkyPipeline != nullptr, "environment sky pipeline");
			CreateOcclusionTargets();
			m_Output = output;
		}

		GpuMesh& GetMesh(const AssetImporter& importer, const std::string& path)
		{
			const auto found = m_Meshes.find(path);
			if (found != m_Meshes.end())
			{
				return *found->second;
			}
			const auto asset = importer.LoadMesh(path);
			auto mesh = std::make_unique<GpuMesh>();
			// Allocate all resources before recording uploads so failures leave no open command list.
			std::vector<std::array<std::vector<ImageAsset>, 2>> mipChains(asset.Textures.size());
			for (size_t textureIndex = 0; textureIndex < asset.Textures.size(); ++textureIndex)
			{
				const auto& texture = asset.Textures[textureIndex];
				bool needsLinear = false;
				bool needsColor = false;
				for (const auto& material : asset.Materials)
				{
					for (const auto& reference :
						 {material.BaseColorTexture, material.MetallicRoughnessTexture, material.NormalTexture,
						  material.OcclusionTexture, material.EmissiveTexture})
					{
						if (reference.Texture == static_cast<int32_t>(textureIndex))
						{
							if (reference.SRGB)
							{
								needsColor = true;
							}
							else
							{
								needsLinear = true;
							}
						}
					}
				}
				const bool needsMips = texture.MinFilter >= 9984;
				if (needsMips)
				{
					for (uint32_t color = 0; color < 2; ++color)
					{
						if (!(color ? needsColor : needsLinear))
						{
							continue;
						}
						auto& levels = mipChains[textureIndex][color];
						const ImageAsset* previous = &texture.Image;
						while (previous->Width > 1 || previous->Height > 1)
						{
							auto mip = DownsampleMaterialTexture(*previous, color != 0);
							levels.push_back(std::move(mip));
							previous = &levels.back();
						}
					}
				}
				nvrhi::TextureDesc description;
				description.width = texture.Image.Width;
				description.height = texture.Image.Height;
				description.format = nvrhi::Format::RGBA8_UNORM;
				description.debugName = texture.Name;
				description.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
				GpuTexture gpuTexture;
				if (needsLinear)
				{
					description.mipLevels = 1 + static_cast<uint32_t>(mipChains[textureIndex][0].size());
					gpuTexture.Texture = m_Device->createTexture(description);
					RequireResource(gpuTexture.Texture != nullptr, "glTF linear material texture");
				}
				if (needsColor)
				{
					description.format = nvrhi::Format::SRGBA8_UNORM;
					description.mipLevels = 1 + static_cast<uint32_t>(mipChains[textureIndex][1].size());
					gpuTexture.ColorTexture = m_Device->createTexture(description);
					RequireResource(gpuTexture.ColorTexture != nullptr, "glTF sRGB material texture");
				}
				nvrhi::SamplerDesc sampler;
				sampler.addressU = AddressMode(texture.WrapU);
				sampler.addressV = AddressMode(texture.WrapV);
				sampler.minFilter = texture.MinFilter == 9729 || texture.MinFilter == 9985 || texture.MinFilter == 9987;
				sampler.magFilter = texture.MagFilter == 9729;
				sampler.mipFilter = texture.MinFilter == 9986 || texture.MinFilter == 9987;
				gpuTexture.Sampler = m_Device->createSampler(sampler);
				RequireResource(gpuTexture.Sampler != nullptr, "glTF material sampler");
				mesh->Textures.push_back(std::move(gpuTexture));
			}
			std::vector<MaterialConstants> materialConstants;
			for (const auto& material : asset.Materials)
			{
				GpuMaterial gpuMaterial;
				gpuMaterial.Blend = material.AlphaMode == MaterialAlphaMode::Blend;
				gpuMaterial.DoubleSided = material.DoubleSided;
				nvrhi::BufferDesc buffer;
				buffer.byteSize = sizeof(MaterialConstants);
				buffer.isConstantBuffer = true;
				buffer.debugName = "Aster material constants";
				buffer.enableAutomaticStateTracking(nvrhi::ResourceStates::ConstantBuffer);
				gpuMaterial.Constants = m_Device->createBuffer(buffer);
				RequireResource(gpuMaterial.Constants != nullptr, "material constant buffer");
				MaterialConstants constants;
				constants.BaseColor = material.BaseColorFactor;
				constants.EmissiveMetallic = glm::vec4(material.EmissiveFactor, material.MetallicFactor);
				constants.Parameters = glm::vec4(material.RoughnessFactor, material.AlphaCutoff,
												 static_cast<float>(material.AlphaMode), material.Unlit ? 1.0f : 0.0f);
				nvrhi::BindingSetDesc bindings;
				bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameBuffer),
									 nvrhi::BindingSetItem::ConstantBuffer(1, gpuMaterial.Constants),
									 nvrhi::BindingSetItem::PushConstants(2, sizeof(DrawConstants))};
				const std::array<TextureReference, 5> references{
					material.BaseColorTexture, material.MetallicRoughnessTexture, material.NormalTexture,
					material.OcclusionTexture, material.EmissiveTexture};
				for (uint32_t index = 0; index < references.size(); ++index)
				{
					const auto& reference = references[index];
					const bool hasTexture = reference.Texture >= 0;
					auto* texture = m_WhiteTexture.Get();
					if (hasTexture)
					{
						const auto& gpuTexture = mesh->Textures.at(static_cast<size_t>(reference.Texture));
						texture = reference.SRGB ? gpuTexture.ColorTexture.Get() : gpuTexture.Texture.Get();
					}
					auto* sampler = hasTexture ? mesh->Textures.at(static_cast<size_t>(reference.Texture)).Sampler.Get()
											   : m_LinearSampler.Get();
					bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
						index, texture, reference.SRGB ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM));
					bindings.bindings.push_back(nvrhi::BindingSetItem::Sampler(index, sampler));
					constants.TextureInfo[index] = glm::vec4(static_cast<float>(reference.TexCoord), reference.Scale,
															 hasTexture ? 1.0f : 0.0f, 0.0f);
					for (int column = 0; column < 3; ++column)
					{
						constants.Transforms[index * 3 + static_cast<uint32_t>(column)] =
							glm::vec4(reference.Transform[column], 0.0f);
					}
				}
				gpuMaterial.Bindings = m_Device->createBindingSet(bindings, m_PbrLayout);
				RequireResource(gpuMaterial.Bindings != nullptr, "PBR material bindings");
				materialConstants.push_back(constants);
				mesh->Materials.push_back(std::move(gpuMaterial));
			}
			for (const auto& primitive : asset.Primitives)
			{
				GpuPrimitive gpuPrimitive;
				gpuPrimitive.MaterialIndex = primitive.MaterialIndex;
				gpuPrimitive.Transform = primitive.Transform;
				gpuPrimitive.IndexCount = static_cast<uint32_t>(primitive.Indices.size());
				nvrhi::BufferDesc vertices;
				vertices.byteSize = primitive.Vertices.size() * sizeof(MeshVertex);
				vertices.isVertexBuffer = true;
				vertices.debugName = "Aster mesh vertices";
				vertices.enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer);
				gpuPrimitive.Vertices = m_Device->createBuffer(vertices);
				RequireResource(gpuPrimitive.Vertices != nullptr, "mesh vertex buffer");
				nvrhi::BufferDesc indices;
				indices.byteSize = primitive.Indices.size() * sizeof(uint32_t);
				indices.isIndexBuffer = true;
				indices.debugName = "Aster mesh indices";
				indices.enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer);
				gpuPrimitive.Indices = m_Device->createBuffer(indices);
				RequireResource(gpuPrimitive.Indices != nullptr, "mesh index buffer");
				glm::vec3 minimum(std::numeric_limits<float>::max());
				glm::vec3 maximum(std::numeric_limits<float>::lowest());
				for (const auto& vertex : primitive.Vertices)
				{
					minimum = glm::min(minimum, vertex.Position);
					maximum = glm::max(maximum, vertex.Position);
				}
				gpuPrimitive.Center = minimum * 0.5f + maximum * 0.5f;
				gpuPrimitive.Minimum = minimum;
				gpuPrimitive.Maximum = maximum;
				mesh->Primitives.push_back(std::move(gpuPrimitive));
			}
			m_UploadCommands->open();
			for (size_t index = 0; index < mesh->Textures.size(); ++index)
			{
				const auto& image = asset.Textures[index].Image;
				for (uint32_t color = 0; color < 2; ++color)
				{
					auto* texture =
						color ? mesh->Textures[index].ColorTexture.Get() : mesh->Textures[index].Texture.Get();
					if (!texture)
					{
						continue;
					}
					m_UploadCommands->writeTexture(texture, 0, 0, image.Pixels.data(),
												   static_cast<size_t>(image.Width) * 4);
					const auto& mips = mipChains[index][color];
					for (uint32_t mip = 0; mip < mips.size(); ++mip)
					{
						m_UploadCommands->writeTexture(texture, 0, mip + 1, mips[mip].Pixels.data(),
													   static_cast<size_t>(mips[mip].Width) * 4);
					}
				}
			}
			for (size_t index = 0; index < mesh->Materials.size(); ++index)
			{
				m_UploadCommands->writeBuffer(mesh->Materials[index].Constants, &materialConstants[index],
											  sizeof(MaterialConstants));
			}
			for (size_t index = 0; index < mesh->Primitives.size(); ++index)
			{
				m_UploadCommands->writeBuffer(mesh->Primitives[index].Vertices, asset.Primitives[index].Vertices.data(),
											  asset.Primitives[index].Vertices.size() * sizeof(MeshVertex));
				m_UploadCommands->writeBuffer(mesh->Primitives[index].Indices, asset.Primitives[index].Indices.data(),
											  asset.Primitives[index].Indices.size() * sizeof(uint32_t));
			}
			SubmitUploads();
			return *m_Meshes.emplace(path, std::move(mesh)).first->second;
		}

		nvrhi::DeviceHandle m_Device;
		nvrhi::ShaderHandle m_DepthVertex;
		nvrhi::ShaderHandle m_OcclusionPixel;
		nvrhi::ShaderHandle m_OcclusionBlurPixel;
		nvrhi::BindingLayoutHandle m_OcclusionLayout;
		nvrhi::BufferHandle m_OcclusionBuffer;
		nvrhi::TextureHandle m_OcclusionRaw;
		nvrhi::TextureHandle m_OcclusionFiltered;
		nvrhi::FramebufferHandle m_DepthFramebuffer;
		nvrhi::FramebufferHandle m_OcclusionFramebuffer;
		nvrhi::FramebufferHandle m_OcclusionBlurFramebuffer;
		nvrhi::BindingSetHandle m_OcclusionBindings;
		nvrhi::BindingSetHandle m_OcclusionBlurBindings;
		std::array<nvrhi::GraphicsPipelineHandle, 4> m_DepthPipelines;
		nvrhi::GraphicsPipelineHandle m_OcclusionPipeline;
		nvrhi::GraphicsPipelineHandle m_OcclusionBlurPipeline;
		OcclusionConstants m_Occlusion;
		nvrhi::ShaderHandle m_ShadowVertex;
		nvrhi::ShaderHandle m_DepthPixel;
		nvrhi::InputLayoutHandle m_DepthVertexLayout;
		nvrhi::SamplerHandle m_ShadowSampler;
		nvrhi::TextureHandle m_ShadowTexture;
		std::vector<nvrhi::FramebufferHandle> m_ShadowFramebuffers;
		std::array<nvrhi::GraphicsPipelineHandle, 4> m_ShadowPipelines;
		std::vector<ShadowFace> m_ShadowFaces;
		nvrhi::CommandListHandle m_UploadCommands;
		nvrhi::ShaderHandle m_PbrVertex;
		nvrhi::ShaderHandle m_PbrPixel;
		nvrhi::ShaderHandle m_ToneVertex;
		nvrhi::ShaderHandle m_TonePixel;
		nvrhi::ShaderHandle m_SkyPixel;
		nvrhi::InputLayoutHandle m_VertexLayout;
		nvrhi::BindingLayoutHandle m_PbrLayout;
		nvrhi::BindingLayoutHandle m_ToneLayout;
		nvrhi::BindingLayoutHandle m_EnvironmentLayout;
		nvrhi::BufferHandle m_FrameBuffer;
		nvrhi::SamplerHandle m_LinearSampler;
		nvrhi::SamplerHandle m_EnvironmentSampler;
		std::array<nvrhi::TextureHandle, 4> m_EnvironmentTextures;
		nvrhi::BindingSetHandle m_EnvironmentBindings;
		bool m_HasEnvironment = false;
		std::filesystem::path m_SceneEnvironmentPath;
		nvrhi::TextureHandle m_WhiteTexture;
		nvrhi::TextureHandle m_Output;
		nvrhi::TextureHandle m_Hdr;
		nvrhi::TextureHandle m_Depth;
		nvrhi::FramebufferHandle m_HdrFramebuffer;
		nvrhi::FramebufferHandle m_OutputFramebuffer;
		nvrhi::BindingSetHandle m_ToneBindings;
		std::array<nvrhi::GraphicsPipelineHandle, 8> m_PbrPipelines;
		nvrhi::GraphicsPipelineHandle m_TonePipeline;
		nvrhi::GraphicsPipelineHandle m_SkyPipeline;
		std::filesystem::path m_AssetRoot;
		std::unordered_map<std::string, std::unique_ptr<GpuMesh>> m_Meshes;
		std::vector<DrawItem> m_Draws;
		FrameConstants m_Frame;
		RenderSettings m_Settings;
	};

	SceneRenderPass::SceneRenderPass(nvrhi::IDevice* device) : m_Impl(std::make_unique<Impl>(device)) {}

	SceneRenderPass::~SceneRenderPass() = default;

	void SceneRenderPass::Prepare(const Scene& scene, const AssetImporter& importer, nvrhi::ITexture* output,
								  const RenderSettings& settings)
	{
		m_Impl->Prepare(scene, importer, output, settings);
	}

	void SceneRenderPass::Record(nvrhi::ICommandList* commands)
	{
		m_Impl->Record(commands);
	}

	void SceneRenderPass::InvalidateAssets()
	{
		m_Impl->InvalidateAssets();
	}

	void SceneRenderPass::SetEnvironment(const EnvironmentMaps& environment)
	{
		m_Impl->SetEnvironment(environment);
	}
} // namespace Aster
