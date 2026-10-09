#include "Aster/Assets/AssetImporter.h"
#include "AssetIO.h"

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Aster
{
	namespace
	{
		constexpr size_t s_MaxElements = 10000000;

		struct ParseBudget
		{
			size_t Bytes = 0;
		};

		struct alignas(std::max_align_t) AllocationHeader
		{
			size_t Bytes;
		};

		void* AllocateParseMemory(void* user, cgltf_size bytes)
		{
			auto& budget = *static_cast<ParseBudget*>(user);
			if (bytes > AssetDetail::s_MaxDecodedBytes - sizeof(AllocationHeader) ||
				bytes + sizeof(AllocationHeader) > AssetDetail::s_MaxDecodedBytes - budget.Bytes)
			{
				return nullptr;
			}
			const auto total = bytes + sizeof(AllocationHeader);
			auto* header = static_cast<AllocationHeader*>(std::malloc(total));
			if (!header)
			{
				return nullptr;
			}
			header->Bytes = total;
			budget.Bytes += total;
			return header + 1;
		}

		void FreeParseMemory(void* user, void* allocation)
		{
			if (allocation)
			{
				auto* header = static_cast<AllocationHeader*>(allocation) - 1;
				static_cast<ParseBudget*>(user)->Bytes -= header->Bytes;
				std::free(header);
			}
		}

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::invalid_argument("glTF: " + message);
			}
		}

		using Json = nlohmann::json;

		void ValidateInteger(const Json& value, std::string_view field)
		{
			Require(value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0),
					std::string(field) + " must be a nonnegative integer");
			Require(value.get<uint64_t>() <= std::numeric_limits<uint32_t>::max(),
					std::string(field) + " exceeds the supported integer range");
		}

		void ValidateNumber(const Json& value, std::string_view field)
		{
			Require(value.is_number(), std::string(field) + " must be a number");
			const auto number = value.get<double>();
			Require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(),
					std::string(field) + " must be a finite float");
		}

		void ValidateFields(const Json& object, std::initializer_list<const char*> integers = {},
							std::initializer_list<const char*> numbers = {},
							std::initializer_list<const char*> booleans = {},
							std::initializer_list<const char*> strings = {})
		{
			Require(object.is_object(), "schema entry must be an object");
			for (const auto* field : integers)
			{
				if (object.contains(field))
				{
					ValidateInteger(object.at(field), field);
				}
			}
			for (const auto* field : numbers)
			{
				if (object.contains(field))
				{
					ValidateNumber(object.at(field), field);
				}
			}
			for (const auto* field : booleans)
			{
				if (object.contains(field))
				{
					Require(object.at(field).is_boolean(), std::string(field) + " must be a boolean");
				}
			}
			for (const auto* field : strings)
			{
				if (object.contains(field))
				{
					Require(object.at(field).is_string(), std::string(field) + " must be a string");
				}
			}
		}

		template <typename Callback> void ValidateObject(const Json& object, const char* field, Callback&& callback)
		{
			if (object.contains(field))
			{
				Require(object.at(field).is_object(), std::string(field) + " must be an object");
				callback(object.at(field));
			}
		}

		template <typename Callback> void ValidateArray(const Json& object, const char* field, Callback&& callback)
		{
			if (object.contains(field))
			{
				const auto& values = object.at(field);
				Require(values.is_array(), std::string(field) + " must be an array");
				for (const auto& value : values)
				{
					callback(value);
				}
			}
		}

		void ValidateVector(const Json& object, const char* field, size_t size = 0)
		{
			if (object.contains(field))
			{
				const auto& values = object.at(field);
				Require(values.is_array() && (size == 0 || values.size() == size),
						std::string(field) + " has invalid vector dimensions");
				for (const auto& value : values)
				{
					ValidateNumber(value, field);
				}
			}
		}

		void ValidateTextureReference(const Json& reference)
		{
			ValidateFields(reference, {"index", "texCoord"}, {"scale", "strength"});
			ValidateObject(reference, "extensions",
						   [](const Json& extensions)
						   {
							   ValidateObject(extensions, "KHR_texture_transform",
											  [](const Json& transform)
											  {
												  ValidateFields(transform, {"texCoord"}, {"rotation"});
												  ValidateVector(transform, "offset", 2);
												  ValidateVector(transform, "scale", 2);
											  });
						   });
		}

		void ValidateJsonTypes(std::span<const uint8_t> bytes)
		{
			// cgltf intentionally uses permissive C numeric conversion. Validate the
			// original JSON so fractions, booleans and strings cannot become indices.
			if (bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0)
			{
				Require(bytes.size() >= 20, "truncated GLB JSON header");
				const auto word = [&](size_t offset)
				{
					return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
						   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
						   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
				};
				const auto length = word(12);
				Require(word(16) == 0x4e4f534a && length <= bytes.size() - 20, "invalid GLB JSON chunk");
				bytes = bytes.subspan(20, length);
			}
			Require(bytes.size() <= 64ULL * 1024ULL * 1024ULL, "JSON document exceeds 64 MiB");
			const auto document = Json::parse(bytes.begin(), bytes.end(),
											  [](int depth, Json::parse_event_t, Json&)
											  {
												  Require(depth <= 128, "JSON nesting exceeds 128 levels");
												  return true;
											  });
			ValidateFields(document, {"scene"});
			ValidateObject(document, "asset", [](const Json& asset)
						   { ValidateFields(asset, {}, {}, {}, {"version", "minVersion", "generator", "copyright"}); });
			ValidateArray(document, "buffers",
						  [](const Json& buffer) { ValidateFields(buffer, {"byteLength"}, {}, {}, {"uri", "name"}); });
			ValidateArray(document, "bufferViews", [](const Json& view)
						  { ValidateFields(view, {"buffer", "byteOffset", "byteLength", "byteStride", "target"}); });
			ValidateArray(document, "accessors",
						  [](const Json& accessor)
						  {
							  ValidateFields(accessor, {"bufferView", "byteOffset", "componentType", "count"}, {},
											 {"normalized"}, {"type", "name"});
							  ValidateVector(accessor, "min");
							  ValidateVector(accessor, "max");
							  ValidateObject(
								  accessor, "sparse",
								  [](const Json& sparse)
								  {
									  ValidateFields(sparse, {"count"});
									  ValidateObject(
										  sparse, "indices", [](const Json& indices)
										  { ValidateFields(indices, {"bufferView", "byteOffset", "componentType"}); });
									  ValidateObject(sparse, "values", [](const Json& values)
													 { ValidateFields(values, {"bufferView", "byteOffset"}); });
								  });
						  });
			ValidateArray(document, "images", [](const Json& image)
						  { ValidateFields(image, {"bufferView"}, {}, {}, {"uri", "mimeType", "name"}); });
			ValidateArray(document, "textures",
						  [](const Json& texture) { ValidateFields(texture, {"sampler", "source"}); });
			ValidateArray(document, "samplers", [](const Json& sampler)
						  { ValidateFields(sampler, {"minFilter", "magFilter", "wrapS", "wrapT"}); });
			ValidateArray(document, "meshes",
						  [](const Json& mesh)
						  {
							  ValidateFields(mesh, {}, {}, {}, {"name"});
							  ValidateArray(mesh, "primitives",
											[](const Json& primitive)
											{
												ValidateFields(primitive, {"indices", "material", "mode"});
												ValidateObject(primitive, "attributes",
															   [](const Json& attributes)
															   {
																   for (const auto& [name, value] : attributes.items())
																   {
																	   ValidateInteger(value, name);
																   }
															   });
											});
						  });
			ValidateArray(document, "nodes",
						  [](const Json& node)
						  {
							  ValidateFields(node, {"mesh", "camera", "skin"}, {}, {}, {"name"});
							  ValidateVector(node, "translation", 3);
							  ValidateVector(node, "rotation", 4);
							  ValidateVector(node, "scale", 3);
							  ValidateVector(node, "matrix", 16);
							  ValidateArray(node, "children",
											[](const Json& child) { ValidateInteger(child, "child node"); });
						  });
			ValidateArray(document, "scenes",
						  [](const Json& scene)
						  {
							  ValidateFields(scene);
							  ValidateArray(scene, "nodes",
											[](const Json& node) { ValidateInteger(node, "scene node"); });
						  });
			ValidateArray(
				document, "materials",
				[](const Json& material)
				{
					ValidateFields(material, {}, {"alphaCutoff"}, {"doubleSided"}, {"alphaMode", "name"});
					if (material.contains("alphaMode"))
					{
						const auto& mode = material.at("alphaMode");
						Require(mode == "OPAQUE" || mode == "MASK" || mode == "BLEND",
								"alphaMode must be OPAQUE, MASK or BLEND");
					}
					ValidateVector(material, "emissiveFactor", 3);
					ValidateObject(material, "pbrMetallicRoughness",
								   [](const Json& pbr)
								   {
									   ValidateFields(pbr, {}, {"metallicFactor", "roughnessFactor"});
									   ValidateVector(pbr, "baseColorFactor", 4);
									   ValidateObject(pbr, "baseColorTexture", ValidateTextureReference);
									   ValidateObject(pbr, "metallicRoughnessTexture", ValidateTextureReference);
								   });
					for (const auto* field : {"normalTexture", "occlusionTexture", "emissiveTexture"})
					{
						ValidateObject(material, field, ValidateTextureReference);
					}
					ValidateObject(material, "extensions",
								   [](const Json& extensions)
								   {
									   ValidateObject(extensions, "KHR_materials_emissive_strength",
													  [](const Json& strength)
													  { ValidateFields(strength, {}, {"emissiveStrength"}); });
								   });
				});
		}

		void CheckRange(size_t offset, size_t count, size_t stride, size_t elementSize, size_t available)
		{
			Require(count > 0 && count <= s_MaxElements && stride >= elementSize && elementSize > 0,
					"invalid accessor count or stride");
			Require(offset <= available && elementSize <= available - offset,
					"accessor starts outside its buffer view");
			Require(count - 1 <= (available - offset - elementSize) / stride,
					"accessor extends outside its buffer view");
		}

		bool IsUnsigned(cgltf_component_type type)
		{
			return type == cgltf_component_type_r_8u || type == cgltf_component_type_r_16u ||
				   type == cgltf_component_type_r_32u;
		}

		uint32_t ReadUnsigned(const uint8_t* bytes, cgltf_component_type type)
		{
			uint32_t value = bytes[0];
			if (type != cgltf_component_type_r_8u)
			{
				value |= static_cast<uint32_t>(bytes[1]) << 8;
			}
			if (type == cgltf_component_type_r_32u)
			{
				value |= static_cast<uint32_t>(bytes[2]) << 16;
				value |= static_cast<uint32_t>(bytes[3]) << 24;
			}
			return value;
		}

		const uint8_t* ViewBytes(const cgltf_buffer_view& view)
		{
			return static_cast<const uint8_t*>(view.buffer->data) + view.offset;
		}

		void ValidateStorage(cgltf_data& data)
		{
			for (size_t index = 0; index < data.buffer_views_count; ++index)
			{
				const auto& view = data.buffer_views[index];
				Require(!view.has_meshopt_compression, "EXT_meshopt_compression is not supported");
				Require(view.buffer && view.buffer->data, "buffer view has no loaded buffer");
				Require(view.offset <= view.buffer->size && view.size <= view.buffer->size - view.offset,
						"buffer view extends outside its buffer");
				Require(view.stride == 0 || (view.stride >= 4 && view.stride <= 252 && view.stride % 4 == 0),
						"interleaved buffer stride must be a multiple of four in [4, 252]");
			}
			for (size_t index = 0; index < data.accessors_count; ++index)
			{
				const auto& accessor = data.accessors[index];
				const auto componentSize = cgltf_component_size(accessor.component_type);
				const auto elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
				Require(componentSize > 0 && elementSize > 0 && accessor.count > 0 && accessor.count <= s_MaxElements,
						"invalid accessor component type, shape, or count");
				Require(accessor.component_type != cgltf_component_type_r_32f || !accessor.normalized,
						"floating-point accessors cannot be normalized");
				if (accessor.buffer_view)
				{
					const auto& view = *accessor.buffer_view;
					CheckRange(accessor.offset, accessor.count, accessor.stride, elementSize, view.size);
					Require((view.offset + accessor.offset) % componentSize == 0 &&
								accessor.stride % componentSize == 0,
							"accessor data is misaligned");
				}
				else
				{
					Require(accessor.is_sparse, "accessor has neither a buffer view nor sparse data");
				}
				if (accessor.is_sparse)
				{
					const auto& sparse = accessor.sparse;
					Require(sparse.indices_buffer_view && sparse.values_buffer_view && sparse.count <= accessor.count &&
								IsUnsigned(sparse.indices_component_type),
							"invalid sparse accessor metadata");
					const auto indexSize = cgltf_component_size(sparse.indices_component_type);
					CheckRange(sparse.indices_byte_offset, sparse.count, indexSize, indexSize,
							   sparse.indices_buffer_view->size);
					CheckRange(sparse.values_byte_offset, sparse.count, elementSize, elementSize,
							   sparse.values_buffer_view->size);
					Require((sparse.indices_buffer_view->offset + sparse.indices_byte_offset) % indexSize == 0 &&
								(sparse.values_buffer_view->offset + sparse.values_byte_offset) % componentSize == 0,
							"sparse accessor data is misaligned");
					Require(sparse.indices_buffer_view->stride == 0 && sparse.values_buffer_view->stride == 0,
							"sparse accessor buffer views cannot be interleaved");
					const auto* indices = ViewBytes(*sparse.indices_buffer_view) + sparse.indices_byte_offset;
					uint32_t previous = 0;
					for (size_t sparseIndex = 0; sparseIndex < sparse.count; ++sparseIndex)
					{
						const auto value =
							ReadUnsigned(indices + sparseIndex * indexSize, sparse.indices_component_type);
						Require(value < accessor.count && (sparseIndex == 0 || value > previous),
								"sparse indices must be increasing and within the accessor");
						previous = value;
					}
				}
			}
		}

		void ValidateNodes(cgltf_data& data)
		{
			Require(data.nodes_count <= 100000, "node count exceeds supported limit");
			std::vector<uint8_t> state(data.nodes_count, 0);
			std::vector<uint8_t> parents(data.nodes_count, 0);
			std::vector<size_t> depth(data.nodes_count, 0);
			for (size_t index = 0; index < data.nodes_count; ++index)
			{
				const auto& node = data.nodes[index];
				Require(!(node.has_matrix && (node.has_translation || node.has_rotation || node.has_scale)),
						"node cannot specify both a matrix and TRS");
				Require(!node.skin && !node.has_mesh_gpu_instancing,
						"skinning and EXT_mesh_gpu_instancing are not supported");
				if (node.has_rotation)
				{
					float lengthSquared = 0.0f;
					for (const auto component : node.rotation)
					{
						lengthSquared += component * component;
					}
					Require(std::isfinite(lengthSquared) && std::abs(lengthSquared - 1.0f) < 0.01f,
							"node rotation quaternion must have unit length");
				}
				std::array<float, 16> matrix;
				cgltf_node_transform_local(&node, matrix.data());
				Require(std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); }),
						"node transform contains nonfinite values");
				Require(std::abs(matrix[3]) < 0.000001f && std::abs(matrix[7]) < 0.000001f &&
							std::abs(matrix[11]) < 0.000001f && std::abs(matrix[15] - 1.0f) < 0.000001f,
						"node transform must be affine");
				for (size_t childIndex = 0; childIndex < node.children_count; ++childIndex)
				{
					const auto child = static_cast<size_t>(node.children[childIndex] - data.nodes);
					Require(child < data.nodes_count && parents[child]++ == 0,
							"nodes cannot have duplicate or multiple parents");
				}
			}
			std::vector<size_t> chain;
			for (size_t index = 0; index < data.nodes_count; ++index)
			{
				chain.clear();
				const cgltf_node* current = &data.nodes[index];
				while (current && state[static_cast<size_t>(current - data.nodes)] == 0)
				{
					const auto currentIndex = static_cast<size_t>(current - data.nodes);
					state[currentIndex] = 1;
					chain.push_back(currentIndex);
					Require(chain.size() <= 1024, "node hierarchy exceeds 1024 levels");
					current = current->parent;
				}
				Require(!current || state[static_cast<size_t>(current - data.nodes)] == 2,
						"node hierarchy contains a cycle");
				size_t parentDepth = current ? depth[static_cast<size_t>(current - data.nodes)] : 0;
				for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator)
				{
					Require(++parentDepth <= 1024, "node hierarchy exceeds 1024 levels");
					depth[*iterator] = parentDepth;
					state[*iterator] = 2;
				}
			}
		}

		std::vector<float> Unpack(const cgltf_accessor& accessor, cgltf_type expectedType, size_t expectedCount)
		{
			Require(accessor.type == expectedType && accessor.count == expectedCount,
					"vertex attribute shape or count mismatch");
			const auto components = cgltf_num_components(expectedType);
			std::vector<float> values(expectedCount * components);
			// Sparse values are tightly packed even when the base accessor is interleaved.
			// Decode separately because cgltf 1.15 applies the base stride to sparse values.
			auto base = accessor;
			base.is_sparse = false;
			Require(cgltf_accessor_unpack_floats(&base, values.data(), values.size()) == values.size(),
					"cannot decode vertex accessor");
			if (accessor.is_sparse)
			{
				const auto& sparse = accessor.sparse;
				cgltf_accessor packed{};
				packed.type = accessor.type;
				packed.component_type = accessor.component_type;
				packed.normalized = accessor.normalized;
				packed.count = sparse.count;
				packed.offset = sparse.values_byte_offset;
				packed.stride = cgltf_calc_size(accessor.type, accessor.component_type);
				packed.buffer_view = sparse.values_buffer_view;
				std::vector<float> replacements(sparse.count * components);
				Require(cgltf_accessor_unpack_floats(&packed, replacements.data(), replacements.size()) ==
							replacements.size(),
						"cannot decode sparse vertex values");
				const auto* indices = ViewBytes(*sparse.indices_buffer_view) + sparse.indices_byte_offset;
				const auto indexSize = cgltf_component_size(sparse.indices_component_type);
				for (size_t index = 0; index < sparse.count; ++index)
				{
					const auto destination = ReadUnsigned(indices + index * indexSize, sparse.indices_component_type);
					std::copy_n(replacements.data() + index * components, components,
								values.data() + destination * components);
				}
			}
			Require(std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }),
					"vertex attribute contains nonfinite values");
			return values;
		}

		TextureReference ReadTextureReference(const cgltf_data& data, const cgltf_texture_view& source, bool srgb)
		{
			TextureReference reference;
			reference.SRGB = srgb;
			if (!source.texture)
			{
				return reference;
			}
			reference.Texture = static_cast<int32_t>(source.texture - data.textures);
			const auto coordinate =
				source.has_transform && source.transform.has_texcoord ? source.transform.texcoord : source.texcoord;
			Require(coordinate >= 0 && coordinate <= 1, "only TEXCOORD_0 and TEXCOORD_1 are supported");
			reference.TexCoord = static_cast<uint32_t>(coordinate);
			Require(std::isfinite(source.scale), "texture scale must be finite");
			reference.Scale = source.scale;
			if (source.has_transform)
			{
				const auto& transform = source.transform;
				Require(std::isfinite(transform.offset[0]) && std::isfinite(transform.offset[1]) &&
							std::isfinite(transform.rotation) && std::isfinite(transform.scale[0]) &&
							std::isfinite(transform.scale[1]),
						"texture transform must be finite");
				const float cosine = std::cos(transform.rotation);
				const float sine = std::sin(transform.rotation);
				reference.Transform =
					glm::mat3(glm::vec3(cosine * transform.scale[0], sine * transform.scale[0], 0.0f),
							  glm::vec3(-sine * transform.scale[1], cosine * transform.scale[1], 0.0f),
							  glm::vec3(transform.offset[0], transform.offset[1], 1.0f));
			}
			return reference;
		}

		void ReadMaterials(const cgltf_data& data, MeshAsset& asset)
		{
			Require(data.materials_count < std::numeric_limits<uint32_t>::max(), "too many materials");
			for (size_t index = 0; index < data.materials_count; ++index)
			{
				const auto& source = data.materials[index];
				MaterialAsset material;
				material.Name = source.name ? source.name : "Material";
				if (source.has_pbr_metallic_roughness)
				{
					const auto& pbr = source.pbr_metallic_roughness;
					material.BaseColorFactor = glm::make_vec4(pbr.base_color_factor);
					material.MetallicFactor = pbr.metallic_factor;
					material.RoughnessFactor = pbr.roughness_factor;
					material.BaseColorTexture = ReadTextureReference(data, pbr.base_color_texture, true);
					material.MetallicRoughnessTexture =
						ReadTextureReference(data, pbr.metallic_roughness_texture, false);
				}
				material.NormalTexture = ReadTextureReference(data, source.normal_texture, false);
				material.OcclusionTexture = ReadTextureReference(data, source.occlusion_texture, false);
				material.EmissiveTexture = ReadTextureReference(data, source.emissive_texture, true);
				material.EmissiveFactor = glm::make_vec3(source.emissive_factor);
				if (source.has_emissive_strength)
				{
					Require(std::isfinite(source.emissive_strength.emissive_strength) &&
								source.emissive_strength.emissive_strength >= 0.0f,
							"emissive strength must be finite and nonnegative");
					material.EmissiveFactor *= source.emissive_strength.emissive_strength;
				}
				material.AlphaMode = source.alpha_mode == cgltf_alpha_mode_mask	   ? MaterialAlphaMode::Mask
									 : source.alpha_mode == cgltf_alpha_mode_blend ? MaterialAlphaMode::Blend
																				   : MaterialAlphaMode::Opaque;
				material.AlphaCutoff = source.alpha_cutoff;
				material.DoubleSided = source.double_sided != 0;
				material.Unlit = source.unlit != 0;
				for (int component = 0; component < 4; ++component)
				{
					Require(std::isfinite(material.BaseColorFactor[component]) &&
								material.BaseColorFactor[component] >= 0.0f &&
								material.BaseColorFactor[component] <= 1.0f,
							"base color factor must be in [0, 1]");
				}
				for (int component = 0; component < 3; ++component)
				{
					Require(std::isfinite(material.EmissiveFactor[component]) &&
								material.EmissiveFactor[component] >= 0.0f,
							"emissive factor must be finite and nonnegative");
				}
				Require(std::isfinite(material.MetallicFactor) && material.MetallicFactor >= 0.0f &&
							material.MetallicFactor <= 1.0f && std::isfinite(material.RoughnessFactor) &&
							material.RoughnessFactor >= 0.0f && material.RoughnessFactor <= 1.0f,
						"metallic and roughness factors must be in [0, 1]");
				Require(std::isfinite(material.AlphaCutoff) && material.AlphaCutoff >= 0.0f &&
							material.OcclusionTexture.Scale >= 0.0f && material.OcclusionTexture.Scale <= 1.0f,
						"invalid alpha cutoff or occlusion strength");
				asset.Materials.push_back(std::move(material));
			}
			asset.Materials.emplace_back();
			asset.Materials.back().Name = "Default";
		}

		void GenerateBasis(MeshPrimitive& primitive, bool hasNormals, bool hasTangents, const MaterialAsset& material)
		{
			std::vector<glm::vec3> normals(primitive.Vertices.size(), glm::vec3(0.0f));
			std::vector<glm::vec3> tangents(primitive.Vertices.size(), glm::vec3(0.0f));
			std::vector<glm::vec3> bitangents(primitive.Vertices.size(), glm::vec3(0.0f));
			for (size_t index = 0; index < primitive.Indices.size(); index += 3)
			{
				const uint32_t a = primitive.Indices[index];
				const uint32_t b = primitive.Indices[index + 1];
				const uint32_t c = primitive.Indices[index + 2];
				const auto edge1 = primitive.Vertices[b].Position - primitive.Vertices[a].Position;
				const auto edge2 = primitive.Vertices[c].Position - primitive.Vertices[a].Position;
				const auto normal = glm::cross(edge1, edge2);
				for (const auto vertex : {a, b, c})
				{
					normals[vertex] += normal;
				}
				auto getUV = [&](uint32_t vertex)
				{
					const auto uv = material.NormalTexture.TexCoord == 1 ? primitive.Vertices[vertex].TexCoord1
																		 : primitive.Vertices[vertex].TexCoord;
					return glm::vec2(material.NormalTexture.Transform * glm::vec3(uv, 1.0f));
				};
				const auto uv1 = getUV(b) - getUV(a);
				const auto uv2 = getUV(c) - getUV(a);
				const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
				if (std::abs(determinant) > 0.00000001f)
				{
					const auto tangent = (edge1 * uv2.y - edge2 * uv1.y) / determinant;
					const auto bitangent = (edge2 * uv1.x - edge1 * uv2.x) / determinant;
					for (const auto vertex : {a, b, c})
					{
						tangents[vertex] += tangent;
						bitangents[vertex] += bitangent;
					}
				}
			}
			for (size_t index = 0; index < primitive.Vertices.size(); ++index)
			{
				auto& vertex = primitive.Vertices[index];
				const auto candidateNormal = hasNormals ? vertex.Normal : normals[index];
				const float normalLength = glm::length(candidateNormal);
				Require(std::isfinite(normalLength), "generated normal exceeds finite range");
				Require(!hasNormals || normalLength > 0.000001f, "authored normals must not be zero");
				vertex.Normal = normalLength > 0.000001f ? candidateNormal / normalLength : glm::vec3(0.0f, 0.0f, 1.0f);
				if (!hasTangents)
				{
					auto tangent = tangents[index] - vertex.Normal * glm::dot(vertex.Normal, tangents[index]);
					float tangentLength = glm::length(tangent);
					Require(std::isfinite(tangentLength), "generated tangent exceeds finite range");
					if (tangentLength <= 0.000001f)
					{
						const auto axis = std::abs(vertex.Normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
																		   : glm::vec3(1.0f, 0.0f, 0.0f);
						tangent = glm::cross(axis, vertex.Normal);
						tangentLength = glm::length(tangent);
					}
					tangent /= tangentLength;
					vertex.Tangent = glm::vec4(
						tangent, glm::dot(glm::cross(vertex.Normal, tangent), bitangents[index]) < 0.0f ? -1.0f : 1.0f);
				}
				else
				{
					const float length = glm::length(glm::vec3(vertex.Tangent));
					Require(std::isfinite(length) && length > 0.000001f &&
								std::abs(std::abs(vertex.Tangent.w) - 1.0f) < 0.0001f,
							"authored tangent must have nonzero direction and handedness +/-1");
					vertex.Tangent = glm::vec4(glm::vec3(vertex.Tangent) / length, vertex.Tangent.w);
				}
			}
		}

		MeshPrimitive ReadPrimitive(const cgltf_primitive& source, const cgltf_data& data, const MeshAsset& asset,
									const glm::mat4& transform, const std::string& name)
		{
			Require(source.type == cgltf_primitive_type_triangles, "only triangle primitives are supported");
			Require(!source.has_draco_mesh_compression && source.targets_count == 0,
					"Draco compression and morph targets are not supported");
			const auto* positions = cgltf_find_accessor(&source, cgltf_attribute_type_position, 0);
			Require(positions && positions->type == cgltf_type_vec3 &&
						positions->component_type == cgltf_component_type_r_32f,
					"POSITION must be a floating-point VEC3 accessor");
			Require(positions->count <= AssetDetail::s_MaxDecodedBytes / (sizeof(MeshVertex) + 64),
					"mesh vertex workspace exceeds the memory limit");
			MeshPrimitive primitive;
			primitive.NodeName = name;
			primitive.Transform = transform;
			primitive.MaterialIndex = source.material ? static_cast<uint32_t>(source.material - data.materials)
													  : static_cast<uint32_t>(data.materials_count);
			primitive.Vertices.resize(positions->count);
			const auto values = Unpack(*positions, cgltf_type_vec3, positions->count);
			for (size_t index = 0; index < positions->count; ++index)
			{
				primitive.Vertices[index].Position = glm::make_vec3(values.data() + index * 3);
			}
			bool hasNormals = false;
			bool hasTangents = false;
			std::array<bool, 2> hasUV{false, false};
			std::unordered_set<std::string> attributes;
			for (size_t attributeIndex = 0; attributeIndex < source.attributes_count; ++attributeIndex)
			{
				const auto& attribute = source.attributes[attributeIndex];
				Require(attribute.name && attributes.insert(attribute.name).second, "duplicate vertex attribute");
				const auto& accessor = *attribute.data;
				if (attribute.type == cgltf_attribute_type_normal || attribute.type == cgltf_attribute_type_tangent)
				{
					const bool normal = attribute.type == cgltf_attribute_type_normal;
					Require(accessor.component_type == cgltf_component_type_r_32f,
							"normal and tangent attributes must use floats");
					const auto unpacked =
						Unpack(accessor, normal ? cgltf_type_vec3 : cgltf_type_vec4, positions->count);
					for (size_t index = 0; index < positions->count; ++index)
					{
						if (normal)
						{
							primitive.Vertices[index].Normal = glm::make_vec3(unpacked.data() + index * 3);
						}
						else
						{
							primitive.Vertices[index].Tangent = glm::make_vec4(unpacked.data() + index * 4);
						}
					}
					hasNormals |= normal;
					hasTangents |= !normal;
				}
				else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index >= 0 &&
						 attribute.index <= 1)
				{
					Require(accessor.component_type == cgltf_component_type_r_32f ||
								(accessor.normalized && (accessor.component_type == cgltf_component_type_r_8u ||
														 accessor.component_type == cgltf_component_type_r_16u)),
							"texture coordinates must be floats or normalized unsigned bytes/shorts");
					const auto unpacked = Unpack(accessor, cgltf_type_vec2, positions->count);
					for (size_t index = 0; index < positions->count; ++index)
					{
						auto& coordinate = attribute.index == 0 ? primitive.Vertices[index].TexCoord
																: primitive.Vertices[index].TexCoord1;
						coordinate = glm::make_vec2(unpacked.data() + index * 2);
					}
					hasUV[static_cast<size_t>(attribute.index)] = true;
				}
				else if (attribute.type == cgltf_attribute_type_color && attribute.index == 0)
				{
					Require(accessor.type == cgltf_type_vec3 || accessor.type == cgltf_type_vec4,
							"vertex color must be VEC3 or VEC4");
					Require(accessor.component_type == cgltf_component_type_r_32f ||
								(accessor.normalized && (accessor.component_type == cgltf_component_type_r_8u ||
														 accessor.component_type == cgltf_component_type_r_16u)),
							"vertex color must be floats or normalized unsigned bytes/shorts");
					const auto unpacked = Unpack(accessor, accessor.type, positions->count);
					const auto components = cgltf_num_components(accessor.type);
					for (size_t index = 0; index < positions->count; ++index)
					{
						for (size_t component = 0; component < components; ++component)
						{
							const float value = unpacked[index * components + component];
							Require(value >= 0.0f && value <= 1.0f, "vertex colors must be in [0, 1]");
							primitive.Vertices[index].Color[static_cast<int>(component)] = value;
						}
					}
				}
			}
			const auto& material = asset.Materials[primitive.MaterialIndex];
			for (const auto* texture : {&material.BaseColorTexture, &material.MetallicRoughnessTexture,
										&material.NormalTexture, &material.OcclusionTexture, &material.EmissiveTexture})
			{
				Require(texture->Texture < 0 || hasUV[texture->TexCoord],
						"material references a missing texture coordinate set");
			}
			if (source.indices)
			{
				const auto& indices = *source.indices;
				Require(indices.type == cgltf_type_scalar && IsUnsigned(indices.component_type) && !indices.normalized,
						"indices must be unsigned, nonnormalized scalar values");
				primitive.Indices.resize(indices.count, 0);
				const auto indexSize = cgltf_component_size(indices.component_type);
				if (indices.buffer_view)
				{
					const auto* bytes = ViewBytes(*indices.buffer_view) + indices.offset;
					for (size_t index = 0; index < indices.count; ++index)
					{
						primitive.Indices[index] = ReadUnsigned(bytes + index * indices.stride, indices.component_type);
					}
				}
				if (indices.is_sparse)
				{
					const auto& sparse = indices.sparse;
					const auto* sparseIndices = ViewBytes(*sparse.indices_buffer_view) + sparse.indices_byte_offset;
					const auto* sparseValues = ViewBytes(*sparse.values_buffer_view) + sparse.values_byte_offset;
					const auto sparseIndexSize = cgltf_component_size(sparse.indices_component_type);
					for (size_t index = 0; index < sparse.count; ++index)
					{
						const auto destination =
							ReadUnsigned(sparseIndices + index * sparseIndexSize, sparse.indices_component_type);
						primitive.Indices[destination] =
							ReadUnsigned(sparseValues + index * indexSize, indices.component_type);
					}
				}
			}
			else
			{
				primitive.Indices.resize(positions->count);
				std::iota(primitive.Indices.begin(), primitive.Indices.end(), 0U);
			}
			Require(primitive.Indices.size() % 3 == 0, "triangle index count must be a multiple of three");
			for (const auto index : primitive.Indices)
			{
				Require(index < primitive.Vertices.size(), "triangle index exceeds vertex count");
			}
			GenerateBasis(primitive, hasNormals, hasTangents, material);
			return primitive;
		}
	} // namespace

	MeshAsset AssetImporter::LoadMesh(const std::filesystem::path& relativePath) const
	{
		const auto sourcePath = ResolvePath(relativePath);
		const auto file = AssetDetail::ReadBytes(sourcePath);
		ValidateJsonTypes(file);
		ParseBudget parseBudget;
		cgltf_options options{};
		options.memory = {AllocateParseMemory, FreeParseMemory, &parseBudget};
		cgltf_data* parsed = nullptr;
		const auto result = cgltf_parse(&options, file.data(), file.size(), &parsed);
		std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
		Require(result == cgltf_result_success && data,
				"cannot parse glTF/GLB document (code " + std::to_string(result) + ")");
		Require(data->asset.version && std::string_view(data->asset.version) == "2.0", "only glTF 2.0 is supported");
		Require(!data->asset.min_version || std::string_view(data->asset.min_version) == "2.0",
				"unsupported glTF minimum version");
		Require(data->animations_count == 0, "animated glTF assets are not supported by the static mesh importer");
		for (size_t index = 0; index < data->extensions_required_count; ++index)
		{
			const std::string_view extension(data->extensions_required[index]);
			Require(extension == "KHR_texture_transform" || extension == "KHR_materials_unlit" ||
						extension == "KHR_materials_emissive_strength",
					"unsupported required extension: " + std::string(extension));
		}
		MeshAsset asset;
		asset.Dependencies.push_back(sourcePath.lexically_relative(m_ProjectRoot));
		std::vector<std::vector<uint8_t>> buffers(data->buffers_count);
		size_t loadedBytes = file.size();
		for (size_t index = 0; index < data->buffers_count; ++index)
		{
			auto& buffer = data->buffers[index];
			Require(buffer.size > 0 && buffer.size <= AssetDetail::s_MaxAssetBytes, "invalid buffer size");
			if (buffer.uri)
			{
				buffers[index] = AssetDetail::ReadURI(m_ProjectRoot, sourcePath, buffer.uri, asset.Dependencies);
			}
			else
			{
				Require(index == 0 && data->bin && data->bin_size >= buffer.size,
						"missing or truncated GLB binary buffer");
				const auto* bytes = static_cast<const uint8_t*>(data->bin);
				buffers[index].assign(bytes, bytes + buffer.size);
			}
			Require(buffers[index].size() >= buffer.size, "buffer file is shorter than its declared byte length");
			loadedBytes += buffers[index].size();
			Require(loadedBytes <= AssetDetail::s_MaxDecodedBytes, "combined asset buffers exceed the memory limit");
			buffer.data = buffers[index].data();
			buffer.data_free_method = cgltf_data_free_method_none;
		}
		ValidateStorage(*data);
		ValidateNodes(*data);
		Require(cgltf_validate(data.get()) == cgltf_result_success,
				"invalid glTF structure, accessor bounds, or node graph");
		Require(data->textures_count <= 16384, "texture count exceeds supported limit");
		for (size_t index = 0; index < data->textures_count; ++index)
		{
			const auto& source = data->textures[index];
			Require(source.image, "texture requires a PNG/JPEG fallback image");
			const auto& image = *source.image;
			Require(!(image.uri && image.buffer_view), "image must have exactly one data source");
			TextureAsset texture;
			texture.Name = source.name ? source.name : image.name ? image.name : "Texture";
			if (image.uri)
			{
				texture.Image = AssetDetail::DecodeImage(
					AssetDetail::ReadURI(m_ProjectRoot, sourcePath, image.uri, asset.Dependencies));
			}
			else
			{
				Require(image.buffer_view, "image has no URI or buffer view");
				texture.Image = AssetDetail::DecodeImage({ViewBytes(*image.buffer_view), image.buffer_view->size});
			}
			loadedBytes += texture.Image.Pixels.size();
			Require(loadedBytes <= AssetDetail::s_MaxDecodedBytes, "decoded textures exceed the asset memory limit");
			if (source.sampler)
			{
				const auto& sampler = *source.sampler;
				texture.MinFilter = sampler.min_filter == 0 ? 9987 : sampler.min_filter;
				texture.MagFilter = sampler.mag_filter == 0 ? 9729 : sampler.mag_filter;
				texture.WrapU = sampler.wrap_s;
				texture.WrapV = sampler.wrap_t;
				Require(texture.MagFilter == 9728 || texture.MagFilter == 9729, "unsupported magnification filter");
				Require(texture.MinFilter == 9728 || texture.MinFilter == 9729 ||
							(texture.MinFilter >= 9984 && texture.MinFilter <= 9987),
						"unsupported minification filter");
				for (const auto wrap : {texture.WrapU, texture.WrapV})
				{
					Require(wrap == 33071 || wrap == 33648 || wrap == 10497, "unsupported texture wrapping mode");
				}
			}
			asset.Textures.push_back(std::move(texture));
		}
		ReadMaterials(*data, asset);
		auto readMesh = [&](const cgltf_mesh& mesh, const glm::mat4& transform, const std::string& name)
		{
			for (size_t index = 0; index < mesh.primitives_count; ++index)
			{
				auto primitive = ReadPrimitive(mesh.primitives[index], *data, asset, transform, name);
				loadedBytes +=
					primitive.Vertices.size() * sizeof(MeshVertex) + primitive.Indices.size() * sizeof(uint32_t);
				Require(loadedBytes <= AssetDetail::s_MaxDecodedBytes, "decoded mesh exceeds the asset memory limit");
				asset.Primitives.push_back(std::move(primitive));
			}
		};
		std::vector<const cgltf_node*> nodes;
		const auto* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
		if (scene)
		{
			nodes.assign(scene->nodes, scene->nodes + scene->nodes_count);
		}
		else
		{
			for (size_t index = 0; index < data->nodes_count; ++index)
			{
				if (!data->nodes[index].parent)
				{
					nodes.push_back(&data->nodes[index]);
				}
			}
		}
		std::unordered_set<const cgltf_node*> visited;
		for (size_t index = 0; index < nodes.size(); ++index)
		{
			const auto& node = *nodes[index];
			Require(visited.insert(&node).second, "scene contains duplicate nodes");
			if (node.mesh)
			{
				glm::mat4 transform;
				cgltf_node_transform_world(&node, glm::value_ptr(transform));
				for (int column = 0; column < 4; ++column)
				{
					for (int row = 0; row < 4; ++row)
					{
						Require(std::isfinite(transform[column][row]), "world transform exceeds finite range");
					}
				}
				Require(std::abs(glm::determinant(glm::mat3(transform))) > 0.00000001f, "mesh transform is singular");
				readMesh(*node.mesh, transform, node.name ? node.name : "Node");
			}
			for (size_t childIndex = 0; childIndex < node.children_count; ++childIndex)
			{
				nodes.push_back(node.children[childIndex]);
			}
		}
		if (data->nodes_count == 0 && !scene)
		{
			for (size_t index = 0; index < data->meshes_count; ++index)
			{
				readMesh(data->meshes[index], glm::mat4(1.0f),
						 data->meshes[index].name ? data->meshes[index].name : "Mesh");
			}
		}
		Require(!asset.Primitives.empty(), "selected glTF scene contains no triangle mesh primitives");
		return asset;
	}
} // namespace Aster
