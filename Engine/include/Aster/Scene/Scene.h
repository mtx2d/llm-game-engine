#pragma once

#include "Aster/Scene/Components.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aster
{
	struct SceneEnvironment
	{
		// Empty path disables image-based lighting. Rotation is yaw in radians.
		std::string Path;
		float Intensity = 1.0f;
		float Rotation = 0.0f;
	};

	struct Entity
	{
		uint32_t Index = std::numeric_limits<uint32_t>::max();
		uint32_t Generation = 0;
		uint64_t SceneID = 0;
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return SceneID != 0;
		}
		bool operator==(const Entity&) const = default;
	};

	class Scene
	{
	  public:
		explicit Scene(std::string name = "Untitled");
		~Scene() = default;
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		Scene(Scene&& other) noexcept;
		Scene& operator=(Scene&& other) noexcept;

		[[nodiscard]] const std::string& GetName() const noexcept
		{
			return m_Name;
		}
		void SetName(std::string name);
		[[nodiscard]] const SceneEnvironment& GetEnvironment() const noexcept
		{
			return m_Environment;
		}
		void SetEnvironment(SceneEnvironment environment);
		[[nodiscard]] Entity CreateEntity(std::string name = "Entity");
		// Destroys the entire descendant subtree. Stale handles are rejected.
		void DestroyEntity(Entity entity);
		[[nodiscard]] bool IsAlive(Entity entity) const noexcept;
		[[nodiscard]] size_t Size() const noexcept
		{
			return m_EntityByID.size();
		}
		// Snapshot permits creation/destruction during iteration; check IsAlive before access.
		[[nodiscard]] std::vector<Entity> Entities() const;
		[[nodiscard]] EntityData& Get(Entity entity);
		[[nodiscard]] const EntityData& Get(Entity entity) const;
		[[nodiscard]] uint64_t GetPersistentID(Entity entity) const;
		[[nodiscard]] Entity FindByID(uint64_t persistentID) const noexcept;
		[[nodiscard]] Entity GetParent(Entity entity) const;
		void SetParent(Entity child, Entity parent = {});
		[[nodiscard]] glm::mat4 GetWorldTransform(Entity entity) const;
		[[nodiscard]] Entity InstantiatePrefab(const Scene& prefab, Entity sourceRoot, Entity parent = {});

		// Direct component edits must pass Validate before persistence or subsystem startup.
		void Validate() const;
		static void ValidateEntityData(const EntityData& data);
		static void ValidateAssetPath(const std::string& path);
		[[nodiscard]] nlohmann::json Serialize() const;
		[[nodiscard]] static Scene Deserialize(const nlohmann::json& document);
		void ReplaceFromJson(const nlohmann::json& document);
		void Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Scene Load(const std::filesystem::path& path);

	  private:
		struct Slot
		{
			uint32_t Generation = 1;
			uint64_t PersistentID = 0;
			Entity Parent;
			std::unique_ptr<EntityData> Data;
		};

		[[nodiscard]] Entity CreateWithID(std::string name, uint64_t persistentID);
		[[nodiscard]] const Slot& GetSlot(Entity entity) const;
		[[nodiscard]] Slot& GetSlot(Entity entity);
		[[nodiscard]] std::vector<Entity> GetSubtree(Entity root) const;
		void Swap(Scene& other) noexcept;

		std::string m_Name;
		SceneEnvironment m_Environment;
		uint64_t m_SceneID;
		uint64_t m_NextPersistentID = 1;
		std::vector<Slot> m_Slots;
		std::vector<uint32_t> m_FreeSlots;
		std::unordered_map<uint64_t, Entity> m_EntityByID;
	};
} // namespace Aster
