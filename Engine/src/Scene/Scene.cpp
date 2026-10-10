#include "Aster/Scene/Scene.h"

#include <Aster/Assets/AssetPath.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Aster
{
	namespace
	{
		std::atomic<uint64_t> s_NextSceneID{1};
		constexpr uint64_t s_MaxPersistentID = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::invalid_argument(message);
			}
		}

		bool IsFinite(float value)
		{
			return std::isfinite(value);
		}
		bool IsFinite(const glm::vec3& value)
		{
			return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
		}

		bool IsFinite(const glm::vec4& value)
		{
			return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
		}

		bool InRange(float value, float minimum, float maximum)
		{
			return IsFinite(value) && value >= minimum && value <= maximum;
		}

		void ValidateName(const std::string& name)
		{
			Require(!name.empty() && name.size() <= 4096 && name.find('\0') == std::string::npos,
					"Names must contain between 1 and 4096 bytes and no null characters");
		}
	} // namespace

	glm::mat4 TransformComponent::GetMatrix() const
	{
		return glm::translate(glm::mat4(1.0f), Translation) *
			   glm::rotate(glm::mat4(1.0f), Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) *
			   glm::rotate(glm::mat4(1.0f), Rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
			   glm::rotate(glm::mat4(1.0f), Rotation.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
			   glm::scale(glm::mat4(1.0f), Scale);
	}

	Scene::Scene(std::string name) : m_Name(std::move(name)), m_SceneID(s_NextSceneID.fetch_add(1))
	{
		ValidateName(m_Name);
	}

	Scene::Scene(Scene&& other) noexcept
		: m_Name(std::move(other.m_Name)), m_Environment(std::move(other.m_Environment)),
		  m_SceneID(std::exchange(other.m_SceneID, s_NextSceneID.fetch_add(1))),
		  m_NextPersistentID(std::exchange(other.m_NextPersistentID, 1)), m_Slots(std::move(other.m_Slots)),
		  m_FreeSlots(std::move(other.m_FreeSlots)), m_EntityByID(std::move(other.m_EntityByID))
	{
		other.m_Slots.clear();
		other.m_FreeSlots.clear();
		other.m_EntityByID.clear();
	}

	Scene& Scene::operator=(Scene&& other) noexcept
	{
		if (this != &other)
		{
			Scene moved(std::move(other));
			Swap(moved);
		}
		return *this;
	}

	void Scene::Swap(Scene& other) noexcept
	{
		using std::swap;
		swap(m_Name, other.m_Name);
		swap(m_Environment, other.m_Environment);
		swap(m_SceneID, other.m_SceneID);
		swap(m_NextPersistentID, other.m_NextPersistentID);
		swap(m_Slots, other.m_Slots);
		swap(m_FreeSlots, other.m_FreeSlots);
		swap(m_EntityByID, other.m_EntityByID);
	}

	void Scene::SetName(std::string name)
	{
		ValidateName(name);
		m_Name = std::move(name);
	}

	void Scene::SetEnvironment(SceneEnvironment environment)
	{
		if (!environment.Path.empty())
		{
			ValidateAssetPath(environment.Path);
		}
		Require(std::isfinite(environment.Intensity) && environment.Intensity >= 0.0f &&
					std::isfinite(environment.Rotation),
				"Environment intensity must be finite and nonnegative; rotation must be finite");
		m_Environment = std::move(environment);
	}

	Entity Scene::CreateEntity(std::string name)
	{
		return CreateWithID(std::move(name), m_NextPersistentID);
	}

	Entity Scene::CreateWithID(std::string name, uint64_t persistentID)
	{
		ValidateName(name);
		Require(Size() < 100000, "Scene supports at most 100000 entities");
		Require(persistentID > 0 && persistentID <= s_MaxPersistentID, "Entity ID is outside supported range");
		Require(!m_EntityByID.contains(persistentID), "Duplicate entity ID");
		auto data = std::make_unique<EntityData>();
		data->Name = std::move(name);
		const bool append = m_FreeSlots.empty();
		Require(!append || m_Slots.size() < std::numeric_limits<uint32_t>::max(), "Scene entity capacity exhausted");
		const auto index = append ? static_cast<uint32_t>(m_Slots.size()) : m_FreeSlots.back();
		if (append)
		{
			m_Slots.emplace_back();
		}
		auto& slot = m_Slots[index];
		const Entity entity{index, slot.Generation, m_SceneID};
		try
		{
			m_EntityByID.emplace(persistentID, entity);
		}
		catch (...)
		{
			if (append)
			{
				m_Slots.pop_back();
			}
			throw;
		}
		if (!append)
		{
			m_FreeSlots.pop_back();
		}
		slot.PersistentID = persistentID;
		slot.Parent = {};
		slot.Data = std::move(data);
		m_NextPersistentID = std::max(m_NextPersistentID, persistentID + 1);
		return entity;
	}

	bool Scene::IsAlive(Entity entity) const noexcept
	{
		return entity.SceneID == m_SceneID && entity.Index < m_Slots.size() &&
			   m_Slots[entity.Index].Generation == entity.Generation && m_Slots[entity.Index].Data;
	}

	const Scene::Slot& Scene::GetSlot(Entity entity) const
	{
		if (!IsAlive(entity))
		{
			throw std::invalid_argument("Entity handle is stale or belongs to another scene");
		}
		return m_Slots[entity.Index];
	}

	Scene::Slot& Scene::GetSlot(Entity entity)
	{
		return const_cast<Slot&>(std::as_const(*this).GetSlot(entity));
	}

	EntityData& Scene::Get(Entity entity)
	{
		return *GetSlot(entity).Data;
	}

	const EntityData& Scene::Get(Entity entity) const
	{
		return *GetSlot(entity).Data;
	}

	uint64_t Scene::GetPersistentID(Entity entity) const
	{
		return GetSlot(entity).PersistentID;
	}

	Entity Scene::GetParent(Entity entity) const
	{
		return GetSlot(entity).Parent;
	}

	Entity Scene::FindByID(uint64_t persistentID) const noexcept
	{
		const auto found = m_EntityByID.find(persistentID);
		return found == m_EntityByID.end() ? Entity{} : found->second;
	}

	std::vector<Entity> Scene::Entities() const
	{
		std::vector<Entity> entities;
		entities.reserve(Size());
		for (size_t index = 0; index < m_Slots.size(); ++index)
		{
			const auto& slot = m_Slots[index];
			if (slot.Data)
			{
				entities.push_back({static_cast<uint32_t>(index), slot.Generation, m_SceneID});
			}
		}
		return entities;
	}

	void Scene::SetParent(Entity child, Entity parent)
	{
		auto& childSlot = GetSlot(child);
		if (parent)
		{
			Entity ancestor = parent;
			while (ancestor)
			{
				Require(ancestor != child, "Reparenting would create a transform cycle");
				ancestor = GetSlot(ancestor).Parent;
			}
		}
		childSlot.Parent = parent;
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity) const
	{
		glm::mat4 world(1.0f);
		Entity current = entity;
		(void)GetSlot(current);
		while (current)
		{
			const auto& slot = GetSlot(current);
			world = slot.Data->Transform.GetMatrix() * world;
			current = slot.Parent;
		}
		return world;
	}

	std::vector<Entity> Scene::GetSubtree(Entity root) const
	{
		(void)GetSlot(root);
		std::vector<std::vector<Entity>> children(m_Slots.size());
		for (const auto candidate : Entities())
		{
			const auto parent = GetParent(candidate);
			if (parent)
			{
				children[parent.Index].push_back(candidate);
			}
		}
		std::vector<Entity> descendants{root};
		for (size_t next = 0; next < descendants.size(); ++next)
		{
			const auto& directChildren = children[descendants[next].Index];
			descendants.insert(descendants.end(), directChildren.begin(), directChildren.end());
		}
		return descendants;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		const auto descendants = GetSubtree(entity);
		// Reserve before mutation so allocation failure cannot partially destroy a subtree.
		m_FreeSlots.reserve(m_FreeSlots.size() + descendants.size());
		for (const auto descendant : descendants)
		{
			auto& slot = GetSlot(descendant);
			m_EntityByID.erase(slot.PersistentID);
			slot.Data.reset();
			slot.Parent = {};
			slot.PersistentID = 0;
			// Retire a saturated slot permanently; never let an old handle become valid again.
			if (slot.Generation < std::numeric_limits<uint32_t>::max())
			{
				++slot.Generation;
				m_FreeSlots.push_back(descendant.Index);
			}
		}
	}

	Entity Scene::InstantiatePrefab(const Scene& prefab, Entity sourceRoot, Entity parent)
	{
		(void)prefab.GetSlot(sourceRoot);
		prefab.Validate();
		if (parent)
		{
			(void)GetSlot(parent);
		}
		const auto sourceEntities = prefab.GetSubtree(sourceRoot);
		std::vector<Entity> created;
		created.reserve(sourceEntities.size());
		std::unordered_map<uint64_t, Entity> remapped;
		remapped.reserve(sourceEntities.size());
		// Reserve rollback storage before any mutation.
		m_FreeSlots.reserve(m_FreeSlots.size() + sourceEntities.size());
		try
		{
			for (const auto source : sourceEntities)
			{
				const EntityData copy = prefab.Get(source);
				const auto clone = CreateEntity(copy.Name);
				created.push_back(clone);
				Get(clone) = copy;
				remapped.emplace(prefab.GetPersistentID(source), clone);
			}
			for (size_t index = 0; index < created.size(); ++index)
			{
				const auto sourceParent = prefab.GetParent(sourceEntities[index]);
				SetParent(created[index], index == 0 ? parent : remapped.at(prefab.GetPersistentID(sourceParent)));
			}
		}
		catch (...)
		{
			// Reverse creation order ensures children are removed before their parent.
			for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
			{
				auto& slot = GetSlot(*iterator);
				m_EntityByID.erase(slot.PersistentID);
				slot.Data.reset();
				slot.Parent = {};
				slot.PersistentID = 0;
				if (slot.Generation < std::numeric_limits<uint32_t>::max())
				{
					++slot.Generation;
					m_FreeSlots.push_back(iterator->Index);
				}
			}
			throw;
		}
		return created.front();
	}

	void Scene::ValidateAssetPath(const std::string& path)
	{
		Require(!path.empty() && path.size() <= 4096, "Asset path must contain 1 to 4096 bytes");
		Require(path.find('\0') == std::string::npos && path.find(':') == std::string::npos,
				"Asset paths cannot contain null characters or drive/scheme prefixes");
		std::string portable = path;
		std::replace(portable.begin(), portable.end(), '\\', '/');
		Require(portable.front() != '/', "Asset paths must be relative to the project");
		const std::filesystem::path relative(portable);
		Require(!ContainsEditorMetadata(relative), "Asset paths cannot reference reserved editor storage");
		for (const auto& segment : relative)
		{
			Require(segment != "..", "Asset paths cannot escape the project");
		}
		Require(relative.has_filename() && relative.filename() != ".", "Asset path must name a file");
	}

	void Scene::ValidateEntityData(const EntityData& data)
	{
		ValidateName(data.Name);
		Require(IsFinite(data.Transform.Translation) && IsFinite(data.Transform.Rotation) &&
					IsFinite(data.Transform.Scale),
				"Transform values must be finite");
		Require(glm::all(glm::greaterThan(glm::abs(data.Transform.Scale), glm::vec3(0.000001f))),
				"Transform scale must be nonsingular");
		if (data.Camera)
		{
			const auto& camera = *data.Camera;
			Require(InRange(camera.VerticalFov, 1.0f, 179.0f) && IsFinite(camera.NearClip) &&
						IsFinite(camera.FarClip) && camera.NearClip > 0.0f && camera.FarClip > camera.NearClip,
					"Invalid camera projection");
		}
		if (data.MeshRenderer)
		{
			const auto& mesh = *data.MeshRenderer;
			ValidateAssetPath(mesh.Mesh);
			Require(IsFinite(mesh.BaseColor) && glm::all(glm::greaterThanEqual(mesh.BaseColor, glm::vec4(0.0f))) &&
						glm::all(glm::lessThanEqual(mesh.BaseColor, glm::vec4(1.0f))),
					"Material base color must be in [0, 1]");
			Require(InRange(mesh.Metallic, 0.0f, 1.0f) && InRange(mesh.Roughness, 0.0f, 1.0f),
					"Invalid PBR material factors");
		}
		if (data.Light)
		{
			const auto& light = *data.Light;
			Require(light.Type == LightType::Directional || light.Type == LightType::Point ||
						light.Type == LightType::Spot,
					"Unsupported light type");
			Require(IsFinite(light.Color) && glm::all(glm::greaterThanEqual(light.Color, glm::vec3(0.0f))) &&
						IsFinite(light.Intensity) && light.Intensity >= 0.0f && IsFinite(light.Range) &&
						light.Range > 0.0f,
					"Invalid light color, intensity, or range");
			Require(InRange(light.InnerCone, 0.0f, 89.0f) && InRange(light.OuterCone, 0.0f, 89.0f) &&
						light.InnerCone <= light.OuterCone,
					"Invalid light cone angles");
		}
		if (data.RigidBody)
		{
			const auto& body = *data.RigidBody;
			Require(body.Type == BodyType::Static || body.Type == BodyType::Kinematic || body.Type == BodyType::Dynamic,
					"Unsupported rigid body type");
			Require(body.Shape == CollisionShape::Box || body.Shape == CollisionShape::Sphere ||
						body.Shape == CollisionShape::Capsule,
					"Unsupported collision shape");
			Require(IsFinite(body.HalfExtents) && glm::all(glm::greaterThan(body.HalfExtents, glm::vec3(0.0f))) &&
						IsFinite(body.Radius) && body.Radius > 0.0f && IsFinite(body.Height) && body.Height > 0.0f,
					"Collision dimensions must be positive and finite");
			Require(IsFinite(body.Mass) && body.Mass >= 0.0f && (body.Type != BodyType::Dynamic || body.Mass > 0.0f),
					"Dynamic bodies require a positive finite mass");
			Require(IsFinite(body.Friction) && body.Friction >= 0.0f && InRange(body.Restitution, 0.0f, 1.0f) &&
						IsFinite(body.LinearVelocity),
					"Invalid rigid body material or velocity");
		}
		if (data.Script)
		{
			ValidateAssetPath(data.Script->Path);
		}
		if (data.AudioSource)
		{
			ValidateAssetPath(data.AudioSource->Path);
			Require(IsFinite(data.AudioSource->Volume) && data.AudioSource->Volume >= 0.0f &&
						IsFinite(data.AudioSource->Pitch) && data.AudioSource->Pitch > 0.0f,
					"Invalid audio volume or pitch");
		}
	}

	void Scene::Validate() const
	{
		ValidateName(m_Name);
		if (!m_Environment.Path.empty())
		{
			ValidateAssetPath(m_Environment.Path);
		}
		Require(std::isfinite(m_Environment.Intensity) && m_Environment.Intensity >= 0.0f &&
					std::isfinite(m_Environment.Rotation),
				"Invalid environment intensity or rotation");
		std::vector<uint8_t> visited(m_Slots.size(), 0);
		std::vector<Entity> ancestry;
		for (const auto entity : Entities())
		{
			ValidateEntityData(Get(entity));
			ancestry.clear();
			Entity current = entity;
			while (current && visited[current.Index] == 0)
			{
				visited[current.Index] = 1;
				ancestry.push_back(current);
				current = GetParent(current);
			}
			Require(!current || visited[current.Index] == 2, "Transform hierarchy contains a cycle");
			for (const auto ancestor : ancestry)
			{
				visited[ancestor.Index] = 2;
			}
		}
	}
} // namespace Aster
