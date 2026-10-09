#include <Aster/Simulation/Simulation.h>

#include <btBulletDynamicsCommon.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#define MINIAUDIO_IMPLEMENTATION
// Device mode must fail when no native output backend is available. Offline
// mixing explicitly sets noDevice and does not need miniaudio's silent backend.
#define MA_NO_NULL
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Aster
{
	namespace
	{
		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::runtime_error(message);
			}
		}

		void CheckVector(glm::vec3 value)
		{
			Require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z),
					"Expected a finite vector");
		}

		btVector3 ToBullet(glm::vec3 value)
		{
			return {value.x, value.y, value.z};
		}
		glm::vec3 FromBullet(const btVector3& value)
		{
			return {value.x(), value.y(), value.z()};
		}

		template <typename Visitor> void Fields(TransformComponent& component, Visitor&& visit)
		{
			visit("Translation", component.Translation);
			visit("Rotation", component.Rotation);
			visit("Scale", component.Scale);
		}
		template <typename Visitor> void Fields(CameraComponent& component, Visitor&& visit)
		{
			visit("VerticalFov", component.VerticalFov);
			visit("NearClip", component.NearClip);
			visit("FarClip", component.FarClip);
			visit("Primary", component.Primary);
		}
		template <typename Visitor> void Fields(MeshRendererComponent& component, Visitor&& visit)
		{
			visit("Mesh", component.Mesh);
			visit("BaseColor", component.BaseColor);
			visit("Metallic", component.Metallic);
			visit("Roughness", component.Roughness);
			visit("Visible", component.Visible);
		}
		template <typename Visitor> void Fields(LightComponent& component, Visitor&& visit)
		{
			visit("Type", component.Type);
			visit("Color", component.Color);
			visit("Intensity", component.Intensity);
			visit("Range", component.Range);
			visit("InnerCone", component.InnerCone);
			visit("OuterCone", component.OuterCone);
			visit("CastShadows", component.CastShadows);
		}
		template <typename Visitor> void Fields(RigidBodyComponent& component, Visitor&& visit)
		{
			visit("Type", component.Type);
			visit("Shape", component.Shape);
			visit("HalfExtents", component.HalfExtents);
			visit("LinearVelocity", component.LinearVelocity);
			visit("Radius", component.Radius);
			visit("Height", component.Height);
			visit("Mass", component.Mass);
			visit("Friction", component.Friction);
			visit("Restitution", component.Restitution);
			visit("IsTrigger", component.IsTrigger);
		}
		template <typename Visitor> void Fields(ScriptComponent& component, Visitor&& visit)
		{
			visit("Path", component.Path);
			visit("Enabled", component.Enabled);
		}
		template <typename Visitor> void Fields(AudioSourceComponent& component, Visitor&& visit)
		{
			visit("Path", component.Path);
			visit("Volume", component.Volume);
			visit("Pitch", component.Pitch);
			visit("Loop", component.Loop);
			visit("PlayOnStart", component.PlayOnStart);
			visit("Spatial", component.Spatial);
		}

		template <typename T> constexpr auto EnumNames();
		template <> constexpr auto EnumNames<BodyType>()
		{
			return std::array{"Static", "Kinematic", "Dynamic"};
		}
		template <> constexpr auto EnumNames<CollisionShape>()
		{
			return std::array{"Box", "Sphere", "Capsule"};
		}
		template <> constexpr auto EnumNames<LightType>()
		{
			return std::array{"Directional", "Point", "Spot"};
		}

		std::string ReadString(lua_State* state, int index)
		{
			Require(lua_type(state, index) == LUA_TSTRING, "Expected a string");
			std::size_t length = 0;
			const char* data = lua_tolstring(state, index, &length);
			Require(length <= 1024 * 1024, "String exceeds the 1 MiB limit");
			Require(std::find(data, data + length, '\0') == data + length, "Embedded NUL is not allowed");
			return std::string(data, length);
		}

		float ReadNumber(lua_State* state, int index)
		{
			Require(lua_type(state, index) == LUA_TNUMBER, "Expected a number");
			const double value = lua_tonumber(state, index);
			Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
					"Expected a finite float");
			return static_cast<float>(value);
		}

		template <typename T> void ReadValue(lua_State* state, int index, T& value)
		{
			if constexpr (std::is_same_v<T, std::string>)
			{
				value = ReadString(state, index);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				Require(lua_isboolean(state, index), "Expected a boolean");
				value = lua_toboolean(state, index) != 0;
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				value = ReadNumber(state, index);
			}
			else if constexpr (std::is_enum_v<T>)
			{
				const auto text = ReadString(state, index);
				const auto names = EnumNames<T>();
				const auto found = std::find(names.begin(), names.end(), text);
				Require(found != names.end(), "Unknown enum value: " + text);
				value = static_cast<T>(std::distance(names.begin(), found));
			}
			else
			{
				Require(lua_istable(state, index), "Expected a numeric vector table");
				index = lua_absindex(state, index);
				Require(lua_rawlen(state, index) == static_cast<std::size_t>(value.length()), "Invalid vector length");
				for (int component = 0; component < value.length(); ++component)
				{
					lua_rawgeti(state, index, component + 1);
					value[component] = ReadNumber(state, -1);
					lua_pop(state, 1);
				}
				lua_pushnil(state);
				while (lua_next(state, index) != 0)
				{
					Require(lua_isinteger(state, -2) && lua_tointeger(state, -2) >= 1 &&
								lua_tointeger(state, -2) <= value.length(),
							"Unknown vector field");
					lua_pop(state, 1);
				}
			}
		}

		template <typename T> void PushValue(lua_State* state, const T& value)
		{
			if constexpr (std::is_same_v<T, std::string>)
			{
				lua_pushlstring(state, value.data(), value.size());
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				lua_pushboolean(state, value);
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				lua_pushnumber(state, value);
			}
			else if constexpr (std::is_enum_v<T>)
			{
				const auto names = EnumNames<T>();
				const auto index = static_cast<std::size_t>(value);
				Require(index < names.size(), "Invalid component enum");
				lua_pushstring(state, names[index]);
			}
			else
			{
				lua_createtable(state, value.length(), 0);
				for (int component = 0; component < value.length(); ++component)
				{
					lua_pushnumber(state, value[component]);
					lua_rawseti(state, -2, component + 1);
				}
			}
		}

		template <typename T> T ReadComponent(lua_State* state, int index)
		{
			Require(lua_istable(state, index), "Expected a component table");
			index = lua_absindex(state, index);
			T component;
			std::set<std::string> names;
			Fields(component,
				   [&](const char* name, auto& field)
				   {
					   names.insert(name);
					   lua_pushstring(state, name);
					   lua_rawget(state, index);
					   if (!lua_isnil(state, -1))
					   {
						   ReadValue(state, -1, field);
					   }
					   lua_pop(state, 1);
				   });
			lua_pushnil(state);
			while (lua_next(state, index) != 0)
			{
				Require(names.contains(ReadString(state, -2)), "Unknown component field");
				lua_pop(state, 1);
			}
			return component;
		}

		template <typename T> void PushComponent(lua_State* state, T component)
		{
			lua_newtable(state);
			Fields(component,
				   [&](const char* name, const auto& field)
				   {
					   PushValue(state, field);
					   lua_setfield(state, -2, name);
				   });
		}

		template <typename T> void PushComponent(lua_State* state, const std::optional<T>& component)
		{
			if (component)
			{
				PushComponent(state, *component);
			}
			else
			{
				lua_pushnil(state);
			}
		}

		bool SameBody(const RigidBodyComponent& first, const RigidBodyComponent& second)
		{
			return first.Type == second.Type && first.Shape == second.Shape &&
				   first.HalfExtents == second.HalfExtents && first.Radius == second.Radius &&
				   first.Height == second.Height && first.Mass == second.Mass && first.Friction == second.Friction &&
				   first.Restitution == second.Restitution && first.IsTrigger == second.IsTrigger;
		}

		bool SameMatrix(const glm::mat4& first, const glm::mat4& second)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (std::abs(first[column][row] - second[column][row]) > 0.00001f)
					{
						return false;
					}
				}
			}
			return true;
		}

		btTransform Decompose(const glm::mat4& matrix, glm::vec3& scale)
		{
			glm::quat rotation;
			glm::vec3 translation, skew;
			glm::vec4 perspective;
			Require(glm::decompose(matrix, scale, rotation, translation, skew, perspective),
					"Cannot decompose physics transform");
			Require(glm::length(skew) < 0.0001f, "Physics does not support sheared transforms");
			Require(scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f, "Physics requires positive world scale");
			return btTransform(btQuaternion(rotation.x, rotation.y, rotation.z, rotation.w), ToBullet(translation));
		}
	} // namespace

	class Simulation::Impl
	{
	  public:
		Impl(Scene& scene, std::filesystem::path assetRoot, SimulationSettings settings)
			: m_Scene(scene), m_AssetRoot(std::filesystem::weakly_canonical(std::move(assetRoot))),
			  m_Settings(settings), m_Dispatcher(&m_CollisionConfiguration),
			  m_World(&m_Dispatcher, &m_Broadphase, &m_Solver, &m_CollisionConfiguration)
		{
			Require(std::isfinite(settings.FixedTimeStep) && settings.FixedTimeStep > 0.0 &&
						settings.FixedTimeStep <= 1.0,
					"FixedTimeStep must be finite and in (0, 1]");
			Require(settings.MaxSubSteps > 0 && settings.MaxSubSteps <= 1000, "MaxSubSteps must be in [1, 1000]");
			Require(settings.Audio == AudioMode::Disabled || settings.Audio == AudioMode::Offline ||
						settings.Audio == AudioMode::Device,
					"Invalid audio mode");
			CheckVector(settings.Gravity);
			Require(std::filesystem::is_directory(m_AssetRoot),
					"Asset root is not a directory: " + m_AssetRoot.string());
			m_World.setGravity(ToBullet(settings.Gravity));
			Require(settings.LuaMemoryLimitBytes >= 1024ULL * 1024ULL &&
						settings.LuaMemoryLimitBytes <= 1024ULL * 1024ULL * 1024ULL,
					"LuaMemoryLimitBytes must be between 1 MiB and 1 GiB");
			m_State = lua_newstate(AllocateLua, this);
			Require(m_State != nullptr, "Unable to create Lua state within its memory limit");
			*static_cast<Impl**>(lua_getextraspace(m_State)) = this;
			try
			{
				const auto initialize = [&]()
				{
					luaL_openlibs(m_State);
					// File access is restricted to the engine's validated asset APIs.
					for (const char* library : {"io", "os", "package", "debug", "dofile", "loadfile", "require"})
					{
						lua_pushnil(m_State);
						lua_setglobal(m_State, library);
					}
					// Lua intentionally disables hooks while running __gc. Reject user
					// finalizers so they cannot bypass instruction limits; engine scripts
					// use OnDestroy for cleanup. Ordinary metatables remain supported.
					lua_getglobal(m_State, "setmetatable");
					lua_pushcclosure(
						m_State,
						[](lua_State* state) -> int
						{
							if (lua_gettop(state) < 2)
							{
								return luaL_error(state, "setmetatable requires a table and a metatable or nil");
							}
							if (lua_istable(state, 2))
							{
								lua_pushliteral(state, "__gc");
								lua_rawget(state, 2);
								const bool finalizer = !lua_isnil(state, -1);
								lua_pop(state, 1);
								if (finalizer)
								{
									return luaL_error(state, "Script __gc finalizers are disabled; use OnDestroy");
								}
							}
							lua_pushvalue(state, lua_upvalueindex(1));
							lua_pushvalue(state, 1);
							lua_pushvalue(state, 2);
							lua_call(state, 2, 1);
							return 1;
						},
						1);
					lua_setglobal(m_State, "setmetatable");
					RegisterBindings();
					return 0;
				};
				if (ProtectedOperation(initialize) != LUA_OK)
				{
					throw std::runtime_error("Unable to initialize Lua: " + LuaError());
				}
				if (settings.Audio != AudioMode::Disabled)
				{
					ma_engine_config configuration = ma_engine_config_init();
					configuration.noDevice = settings.Audio == AudioMode::Offline;
					configuration.channels = 2;
					configuration.sampleRate = 48000;
					const auto result = ma_engine_init(&configuration, &m_AudioEngine);
					Require(result == MA_SUCCESS, std::string(settings.Audio == AudioMode::Device
																  ? "miniaudio device initialization failed: "
																  : "miniaudio offline initialization failed: ") +
													  ma_result_description(result));
					m_AudioInitialized = true;
				}
			}
			catch (...)
			{
				lua_close(m_State);
				m_State = nullptr;
				throw;
			}
		}

		~Impl()
		{
			Stop();
			if (m_AudioInitialized)
			{
				ma_engine_uninit(&m_AudioEngine);
			}
			lua_close(m_State);
		}

		void Start()
		{
			Require(!m_Running, "Simulation is already running");
			m_Scene.Validate();
			SyncBodies();
			SyncAudio();
			m_Running = true;
			m_Accumulator = 0.0;
			SyncScripts();
		}

		void Stop()
		{
			m_Running = false;
			// Remove each registry entry before invoking OnDestroy, so callback-created
			// entities cannot invalidate the traversal or reenter an existing entry.
			while (!m_Scripts.empty())
			{
				auto node = m_Scripts.extract(m_Scripts.begin());
				DestroyScript(node.mapped());
			}
			for (auto& [id, body] : m_Bodies)
			{
				(void)id;
				m_World.removeRigidBody(body.Body.get());
			}
			m_Bodies.clear();
			m_AudioSources.clear();
			m_Contacts.clear();
			m_Accumulator = 0.0;
			m_CurrentInput = {};
			m_PendingInput = {};
		}

		void Update(double deltaTime)
		{
			Require(m_Running, "Simulation is not running");
			Require(std::isfinite(deltaTime) && deltaTime >= 0.0, "Delta time must be finite and nonnegative");
			// Bound catch-up after a debugger pause. Excess wall time is intentionally discarded.
			m_Accumulator +=
				std::min(deltaTime, m_Settings.FixedTimeStep * static_cast<double>(m_Settings.MaxSubSteps));
			std::size_t steps = 0;
			while (m_Accumulator + 1e-12 >= m_Settings.FixedTimeStep && steps < m_Settings.MaxSubSteps)
			{
				Step();
				m_Accumulator -= m_Settings.FixedTimeStep;
				++steps;
			}
		}

		void Step()
		{
			Require(m_Running, "Simulation is not running");
			m_CurrentInput = m_PendingInput;
			m_PendingInput.KeysPressed.clear();
			m_PendingInput.KeysReleased.clear();
			m_PendingInput.MousePressed.fill(false);
			m_PendingInput.MouseReleased.fill(false);
			m_PendingInput.MouseDelta = {0, 0};
			m_PendingInput.Wheel = {0, 0};
			SyncScripts();
			std::vector<std::uint64_t> scripts;
			for (const auto& [id, script] : m_Scripts)
			{
				(void)script;
				scripts.push_back(id);
			}
			for (const auto id : scripts)
			{
				auto found = m_Scripts.find(id);
				if (found != m_Scripts.end() && IsScriptActive(found->second))
				{
					Call(found->second, "OnUpdate", 0, false, true);
				}
			}
			m_Scene.Validate();
			SyncBodies();
			m_World.stepSimulation(static_cast<btScalar>(m_Settings.FixedTimeStep), 0);
			WriteBodyTransforms();
			DispatchContacts();
			SyncAudio();
		}

		void SetInput(const InputSnapshot& input)
		{
			InputState::Validate(input);
			auto pending = m_PendingInput;
			for (const auto& key : pending.KeysDown)
			{
				if (!input.KeysDown.contains(key))
				{
					pending.KeysReleased.insert(key);
				}
			}
			for (const auto& key : input.KeysDown)
			{
				if (!pending.KeysDown.contains(key))
				{
					pending.KeysPressed.insert(key);
				}
			}
			pending.KeysDown = input.KeysDown;
			pending.KeysPressed.insert(input.KeysPressed.begin(), input.KeysPressed.end());
			pending.KeysReleased.insert(input.KeysReleased.begin(), input.KeysReleased.end());
			for (std::size_t index = 0; index < input.MouseDown.size(); ++index)
			{
				pending.MousePressed[index] = pending.MousePressed[index] || input.MousePressed[index] ||
											  (input.MouseDown[index] && !pending.MouseDown[index]);
				pending.MouseReleased[index] = pending.MouseReleased[index] || input.MouseReleased[index] ||
											   (!input.MouseDown[index] && pending.MouseDown[index]);
			}
			pending.MouseDown = input.MouseDown;
			pending.MousePosition = input.MousePosition;
			pending.MouseDelta = input.Focused ? pending.MouseDelta + input.MouseDelta : glm::vec2(0);
			pending.Wheel = input.Focused ? pending.Wheel + input.Wheel : glm::vec2(0);
			pending.Focused = input.Focused;
			InputState::Validate(pending);
			m_PendingInput = std::move(pending);
		}

		void Execute(std::string_view source, std::string_view label)
		{
			Require(source.size() <= 1024 * 1024, "Script source exceeds 1 MiB limit");
			const int top = lua_gettop(m_State);
			const std::string sourceName(label);
			const auto execute = [&]()
			{
				if (luaL_loadbufferx(m_State, source.data(), source.size(), sourceName.c_str(), "t") != LUA_OK)
				{
					return lua_error(m_State);
				}
				lua_call(m_State, 0, 0);
				return 0;
			};
			if (ProtectedOperation(execute) != LUA_OK)
			{
				const std::string error = LuaError();
				lua_settop(m_State, top);
				throw std::runtime_error(sourceName + ": " + error);
			}
			lua_settop(m_State, top);
		}

		void ApplyForce(Entity entity, glm::vec3 force)
		{
			CheckVector(force);
			auto& body = DynamicBody(entity);
			body.activate(true);
			body.applyCentralForce(ToBullet(force));
		}

		void ApplyImpulse(Entity entity, glm::vec3 impulse)
		{
			CheckVector(impulse);
			auto& body = DynamicBody(entity);
			body.activate(true);
			body.applyCentralImpulse(ToBullet(impulse));
			m_Scene.Get(entity).RigidBody->LinearVelocity = FromBullet(body.getLinearVelocity());
		}

		void SetVelocity(Entity entity, glm::vec3 velocity)
		{
			CheckVector(velocity);
			auto& body = DynamicBody(entity);
			body.activate(true);
			body.setLinearVelocity(ToBullet(velocity));
			m_Scene.Get(entity).RigidBody->LinearVelocity = velocity;
		}

		glm::vec3 GetVelocity(Entity entity)
		{
			return FromBullet(DynamicBody(entity).getLinearVelocity());
		}

		void PlayAudio(Entity entity)
		{
			auto& source = GetAudio(entity);
			Require(ma_sound_seek_to_pcm_frame(&source.Sound, 0) == MA_SUCCESS, "Unable to rewind audio");
			Require(ma_sound_start(&source.Sound) == MA_SUCCESS, "Unable to start audio");
		}

		void StopAudio(Entity entity)
		{
			Require(ma_sound_stop(&GetAudio(entity).Sound) == MA_SUCCESS, "Unable to stop audio");
		}

		bool IsAudioPlaying(Entity entity)
		{
			return ma_sound_is_playing(&GetAudio(entity).Sound) != 0;
		}

		std::vector<float> RenderAudio(std::uint32_t frameCount)
		{
			Require(m_Settings.Audio == AudioMode::Offline, "PCM rendering requires offline audio mode");
			Require(frameCount > 0 && frameCount <= 480000, "Audio frame count must be in [1, 480000]");
			SyncAudio();
			std::vector<float> samples(static_cast<std::size_t>(frameCount) * 2);
			ma_uint64 framesRead = 0;
			Require(ma_engine_read_pcm_frames(&m_AudioEngine, samples.data(), frameCount, &framesRead) == MA_SUCCESS,
					"miniaudio mixing failed");
			Require(framesRead == frameCount, "miniaudio produced an incomplete block");
			return samples;
		}

		const std::vector<std::string>& GetErrors() const
		{
			return m_Errors;
		}
		const std::vector<std::string>& GetLog() const
		{
			return m_Log;
		}
		void ClearErrors()
		{
			m_Errors.clear();
		}
		bool IsRunning() const
		{
			return m_Running;
		}

	  private:
		struct BodyRecord
		{
			Entity Handle;
			RigidBodyComponent Configuration;
			glm::mat4 LastTransform{1.0f};
			glm::vec3 Scale{1.0f};
			std::unique_ptr<btCollisionShape> Shape;
			std::unique_ptr<btDefaultMotionState> Motion;
			std::unique_ptr<btRigidBody> Body;
		};

		struct ScriptRecord
		{
			Entity Handle;
			std::uint64_t ID = 0;
			std::string Path;
			int Reference = LUA_NOREF;
			bool Failed = false;
		};

		struct AudioRecord
		{
			Entity Handle;
			std::string Path;
			ma_sound Sound{};
			bool Initialized = false;
			~AudioRecord()
			{
				if (Initialized)
				{
					ma_sound_uninit(&Sound);
				}
			}
		};

		std::filesystem::path ResolveAsset(const std::string& relative) const
		{
			Scene::ValidateAssetPath(relative);
			Require(!relative.empty(), "Asset path must not be empty");
			const auto absolute = std::filesystem::weakly_canonical(m_AssetRoot / relative);
			auto rootIterator = m_AssetRoot.begin();
			auto pathIterator = absolute.begin();
			for (; rootIterator != m_AssetRoot.end(); ++rootIterator, ++pathIterator)
			{
				Require(pathIterator != absolute.end() && *rootIterator == *pathIterator,
						"Asset path escapes asset root");
			}
			Require(std::filesystem::is_regular_file(absolute), "Asset file does not exist: " + relative);
			return absolute;
		}

		btRigidBody& DynamicBody(Entity entity)
		{
			const auto& data = m_Scene.Get(entity);
			Require(data.RigidBody && data.RigidBody->Type == BodyType::Dynamic,
					"Entity requires a dynamic rigid body");
			SyncBodies();
			return *m_Bodies.at(m_Scene.GetPersistentID(entity)).Body;
		}

		void SyncBodies()
		{
			for (auto iterator = m_Bodies.begin(); iterator != m_Bodies.end();)
			{
				if (!m_Scene.IsAlive(iterator->second.Handle) || !m_Scene.Get(iterator->second.Handle).RigidBody)
				{
					m_World.removeRigidBody(iterator->second.Body.get());
					iterator = m_Bodies.erase(iterator);
				}
				else
				{
					++iterator;
				}
			}
			for (const auto entity : m_Scene.Entities())
			{
				const auto& data = m_Scene.Get(entity);
				if (!data.RigidBody)
				{
					continue;
				}
				Scene::ValidateEntityData(data);
				const auto id = m_Scene.GetPersistentID(entity);
				const auto matrix = m_Scene.GetWorldTransform(entity);
				glm::vec3 scale;
				const auto transform = Decompose(matrix, scale);
				const auto& configuration = *data.RigidBody;
				if (configuration.Type == BodyType::Dynamic)
				{
					for (auto ancestor = m_Scene.GetParent(entity); ancestor; ancestor = m_Scene.GetParent(ancestor))
					{
						Require(!m_Scene.Get(ancestor).RigidBody,
								"A dynamic rigid body cannot descend from another rigid body");
					}
				}
				auto found = m_Bodies.find(id);
				if (found != m_Bodies.end() && (!SameBody(found->second.Configuration, configuration) ||
												glm::length(found->second.Scale - scale) > 0.00001f))
				{
					m_World.removeRigidBody(found->second.Body.get());
					m_Bodies.erase(found);
					found = m_Bodies.end();
				}
				if (found == m_Bodies.end())
				{
					BodyRecord record;
					record.Handle = entity;
					record.Configuration = configuration;
					record.Scale = scale;
					record.LastTransform = matrix;
					switch (configuration.Shape)
					{
					case CollisionShape::Box:
						record.Shape = std::make_unique<btBoxShape>(ToBullet(configuration.HalfExtents));
						break;
					case CollisionShape::Sphere:
						record.Shape = std::make_unique<btSphereShape>(configuration.Radius);
						break;
					case CollisionShape::Capsule:
						record.Shape = std::make_unique<btCapsuleShape>(configuration.Radius, configuration.Height);
						break;
					}
					Require(record.Shape != nullptr, "Unsupported collision shape");
					if (configuration.Shape != CollisionShape::Box)
					{
						Require(std::abs(scale.x - scale.y) < 0.0001f && std::abs(scale.x - scale.z) < 0.0001f,
								"Sphere and capsule colliders require uniform world scale");
					}
					record.Shape->setLocalScaling(ToBullet(scale));
					const btScalar mass = configuration.Type == BodyType::Dynamic ? configuration.Mass : 0.0f;
					btVector3 inertia(0, 0, 0);
					if (mass > 0)
					{
						record.Shape->calculateLocalInertia(mass, inertia);
					}
					record.Motion = std::make_unique<btDefaultMotionState>(transform);
					btRigidBody::btRigidBodyConstructionInfo info(mass, record.Motion.get(), record.Shape.get(),
																  inertia);
					info.m_friction = configuration.Friction;
					info.m_restitution = configuration.Restitution;
					record.Body = std::make_unique<btRigidBody>(info);
					record.Body->setLinearVelocity(ToBullet(configuration.LinearVelocity));
					if (configuration.Type == BodyType::Kinematic)
					{
						record.Body->setCollisionFlags(record.Body->getCollisionFlags() |
													   btCollisionObject::CF_KINEMATIC_OBJECT);
						record.Body->setActivationState(DISABLE_DEACTIVATION);
					}
					if (configuration.IsTrigger)
					{
						record.Body->setCollisionFlags(record.Body->getCollisionFlags() |
													   btCollisionObject::CF_NO_CONTACT_RESPONSE);
					}
					auto [inserted, success] = m_Bodies.emplace(id, std::move(record));
					(void)success;
					m_World.addRigidBody(inserted->second.Body.get());
				}
				else
				{
					auto& record = found->second;
					if (!SameMatrix(matrix, record.LastTransform))
					{
						record.Body->setWorldTransform(transform);
						record.Motion->setWorldTransform(transform);
						record.Body->activate(true);
						record.LastTransform = matrix;
						m_World.updateSingleAabb(record.Body.get());
					}
					if (configuration.LinearVelocity != FromBullet(record.Body->getLinearVelocity()))
					{
						record.Body->setLinearVelocity(ToBullet(configuration.LinearVelocity));
						record.Body->activate(true);
					}
				}
			}
		}

		void WriteBodyTransforms()
		{
			// Parent world transforms must be stable for all bodies before deriving locals.
			std::map<std::uint64_t, glm::mat4> worldMatrices;
			for (auto& [id, record] : m_Bodies)
			{
				const auto& transform = record.Body->getWorldTransform();
				const auto& rotation = transform.getRotation();
				glm::mat4 matrix = glm::mat4_cast(glm::quat(rotation.w(), rotation.x(), rotation.y(), rotation.z()));
				for (int axis = 0; axis < 3; ++axis)
				{
					matrix[axis] *= record.Scale[axis];
				}
				matrix[3] = glm::vec4(FromBullet(transform.getOrigin()), 1.0f);
				worldMatrices.emplace(id, matrix);
			}
			for (auto& [id, record] : m_Bodies)
			{
				auto& data = m_Scene.Get(record.Handle);
				if (record.Configuration.Type == BodyType::Dynamic)
				{
					auto local = worldMatrices.at(id);
					const auto parent = m_Scene.GetParent(record.Handle);
					if (parent)
					{
						// Dynamic descendants of another body require a constraint system.
						for (auto ancestor = parent; ancestor; ancestor = m_Scene.GetParent(ancestor))
						{
							Require(!m_Scene.Get(ancestor).RigidBody,
									"A dynamic rigid body cannot descend from another rigid body");
						}
						local = glm::inverse(m_Scene.GetWorldTransform(parent)) * local;
					}
					glm::vec3 localScale;
					const auto transform = Decompose(local, localScale);
					const auto& rotation = transform.getRotation();
					data.Transform.Translation = FromBullet(transform.getOrigin());
					data.Transform.Rotation =
						glm::eulerAngles(glm::quat(rotation.w(), rotation.x(), rotation.y(), rotation.z()));
					data.RigidBody->LinearVelocity = FromBullet(record.Body->getLinearVelocity());
				}
				record.LastTransform = m_Scene.GetWorldTransform(record.Handle);
			}
		}

		static void BudgetHook(lua_State* state, lua_Debug*)
		{
			auto* implementation = *static_cast<Impl**>(lua_getextraspace(state));
			implementation->m_InstructionBudget = std::max(0, implementation->m_InstructionBudget - 1000);
			if (implementation->m_InstructionBudget > 0)
			{
				return;
			}
			// Coroutine threads inherit both this hook and the shared Impl pointer.
			// Exhaustion remains shared even if a child error is caught by resume.
			lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
			auto* mainState = lua_tothread(state, -1);
			lua_pop(state, 1);
			lua_sethook(mainState, BudgetHook, LUA_MASKCOUNT, 1);
			lua_sethook(state, BudgetHook, LUA_MASKCOUNT, 1);
			luaL_error(state, "Script exceeded instruction budget (1000000 instructions)");
		}

		static void* AllocateLua(void* user, void* pointer, size_t previousSize, size_t requestedSize) noexcept
		{
			auto& implementation = *static_cast<Impl*>(user);
			// Lua passes a type tag as previousSize for new allocations.
			const auto oldBytes = pointer ? previousSize : 0;
			if (requestedSize == 0)
			{
				std::free(pointer);
				implementation.m_LuaAllocatedBytes -= oldBytes;
				return nullptr;
			}
			const auto retained = implementation.m_LuaAllocatedBytes - oldBytes;
			if (requestedSize > implementation.m_Settings.LuaMemoryLimitBytes - retained)
			{
				return nullptr;
			}
			auto* resized = std::realloc(pointer, requestedSize);
			if (resized)
			{
				implementation.m_LuaAllocatedBytes = retained + requestedSize;
			}
			return resized;
		}

		template <typename OperationCallback> int ProtectedOperation(const OperationCallback& operation)
		{
			// The light C function and userdata require no heap allocation. All Lua
			// calls that can allocate, including argument/table preparation, execute
			// inside this protected frame. Lua is compiled with C++ unwinding.
			lua_pushcfunction(m_State,
							  [](lua_State* state) -> int
							  {
								  const auto* callback =
									  static_cast<const OperationCallback*>(lua_touserdata(state, 1));
								  lua_remove(state, 1);
								  return (*callback)();
							  });
			lua_pushlightuserdata(m_State, const_cast<OperationCallback*>(&operation));
			m_InstructionBudget = 1000000;
			lua_sethook(m_State, BudgetHook, LUA_MASKCOUNT, 1000);
			const int result = lua_pcall(m_State, 1, 0, 0);
			lua_sethook(m_State, nullptr, 0, 0);
			return result;
		}

		std::string LuaError() const
		{
			const char* message = lua_type(m_State, -1) == LUA_TSTRING ? lua_tostring(m_State, -1) : nullptr;
			return message ? message : "Lua raised a non-string error";
		}

		void Call(ScriptRecord& script, const char* callback, std::uint64_t other = 0, bool began = false,
				  bool update = false)
		{
			if (script.Failed || script.Reference == LUA_NOREF)
			{
				return;
			}
			const int top = lua_gettop(m_State);
			const auto invoke = [&]()
			{
				lua_rawgeti(m_State, LUA_REGISTRYINDEX, script.Reference);
				lua_pushstring(m_State, callback);
				lua_rawget(m_State, -2);
				if (lua_isnil(m_State, -1))
				{
					return 0;
				}
				lua_pushvalue(m_State, -2);
				lua_pushinteger(m_State, static_cast<lua_Integer>(script.ID));
				int arguments = 2;
				if (update)
				{
					lua_pushnumber(m_State, m_Settings.FixedTimeStep);
					++arguments;
				}
				if (other != 0)
				{
					lua_pushinteger(m_State, static_cast<lua_Integer>(other));
					lua_pushboolean(m_State, began);
					arguments += 2;
				}
				lua_call(m_State, arguments, 0);
				return 0;
			};
			if (ProtectedOperation(invoke) != LUA_OK)
			{
				m_Errors.push_back(script.Path + " [" + callback + "]: " + LuaError());
				script.Failed = true;
			}
			lua_settop(m_State, top);
		}

		void DestroyScript(ScriptRecord& script)
		{
			Call(script, "OnDestroy");
			if (script.Reference != LUA_NOREF)
			{
				luaL_unref(m_State, LUA_REGISTRYINDEX, script.Reference);
			}
		}

		bool IsScriptActive(const ScriptRecord& script) const
		{
			if (!m_Scene.IsAlive(script.Handle))
			{
				return false;
			}
			const auto& component = m_Scene.Get(script.Handle).Script;
			return component && component->Enabled && component->Path == script.Path;
		}

		void SyncScripts()
		{
			const auto entities = m_Scene.Entities();
			std::vector<std::uint64_t> removed;
			for (const auto& [id, script] : m_Scripts)
			{
				if (!m_Scene.IsAlive(script.Handle))
				{
					removed.push_back(id);
					continue;
				}
				const auto& component = m_Scene.Get(script.Handle).Script;
				if (!component || !component->Enabled || component->Path != script.Path)
				{
					removed.push_back(id);
				}
			}
			for (const auto id : removed)
			{
				auto node = m_Scripts.extract(id);
				DestroyScript(node.mapped());
			}
			for (const auto entity : entities)
			{
				if (!m_Scene.IsAlive(entity))
				{
					continue;
				}
				const auto component = m_Scene.Get(entity).Script;
				const auto id = m_Scene.GetPersistentID(entity);
				if (!component || !component->Enabled || m_Scripts.contains(id))
				{
					continue;
				}
				ScriptRecord script{entity, id, component->Path, LUA_NOREF, false};
				const int top = lua_gettop(m_State);
				try
				{
					const auto path = ResolveAsset(component->Path);
					Require(std::filesystem::file_size(path) <= 1024 * 1024, "Script exceeds 1 MiB limit");
					std::ifstream stream(path, std::ios::binary);
					Require(static_cast<bool>(stream), "Cannot open script: " + component->Path);
					std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
					const auto load = [&]()
					{
						if (luaL_loadbufferx(m_State, source.data(), source.size(), component->Path.c_str(), "t") !=
							LUA_OK)
						{
							return lua_error(m_State);
						}
						lua_call(m_State, 0, 1);
						if (!lua_istable(m_State, -1))
						{
							return luaL_error(m_State, "Script must return a lifecycle table");
						}
						script.Reference = luaL_ref(m_State, LUA_REGISTRYINDEX);
						return 0;
					};
					if (ProtectedOperation(load) != LUA_OK)
					{
						throw std::runtime_error(LuaError());
					}
				}
				catch (const std::exception& error)
				{
					m_Errors.push_back(component->Path + ": " + error.what());
					script.Failed = true;
				}
				lua_settop(m_State, top);
				auto [inserted, success] = m_Scripts.emplace(id, std::move(script));
				(void)success;
				if (IsScriptActive(inserted->second))
				{
					Call(inserted->second, "OnCreate");
				}
			}
		}

		void DispatchContacts()
		{
			std::map<const btCollisionObject*, std::uint64_t> ids;
			for (const auto& [id, record] : m_Bodies)
			{
				ids.emplace(record.Body.get(), id);
			}
			std::set<std::pair<std::uint64_t, std::uint64_t>> contacts;
			for (int index = 0; index < m_Dispatcher.getNumManifolds(); ++index)
			{
				const auto* manifold = m_Dispatcher.getManifoldByIndexInternal(index);
				bool touching = false;
				for (int point = 0; point < manifold->getNumContacts(); ++point)
				{
					touching |= manifold->getContactPoint(point).getDistance() <= 0.0f;
				}
				if (touching)
				{
					const auto first = ids.at(manifold->getBody0());
					const auto second = ids.at(manifold->getBody1());
					contacts.emplace(std::min(first, second), std::max(first, second));
				}
			}
			auto dispatch = [&](const auto& pair, bool began)
			{
				for (const auto& [self, other] : {pair, std::pair{pair.second, pair.first}})
				{
					const auto found = m_Scripts.find(self);
					if (found != m_Scripts.end() && IsScriptActive(found->second))
					{
						Call(found->second, "OnCollision", other, began);
					}
				}
			};
			for (const auto& pair : contacts)
			{
				if (!m_Contacts.contains(pair))
				{
					dispatch(pair, true);
				}
			}
			for (const auto& pair : m_Contacts)
			{
				if (!contacts.contains(pair))
				{
					dispatch(pair, false);
				}
			}
			m_Contacts = std::move(contacts);
		}

		void SyncAudio()
		{
			if (!m_AudioInitialized)
			{
				return;
			}
			for (auto iterator = m_AudioSources.begin(); iterator != m_AudioSources.end();)
			{
				const auto entity = iterator->second->Handle;
				if (!m_Scene.IsAlive(entity) || !m_Scene.Get(entity).AudioSource ||
					m_Scene.Get(entity).AudioSource->Path != iterator->second->Path)
				{
					iterator = m_AudioSources.erase(iterator);
				}
				else
				{
					++iterator;
				}
			}
			for (const auto entity : m_Scene.Entities())
			{
				const auto& data = m_Scene.Get(entity);
				if (data.Camera && data.Camera->Primary)
				{
					const auto world = m_Scene.GetWorldTransform(entity);
					const auto position = glm::vec3(world[3]);
					const auto forward = -glm::normalize(glm::vec3(world[2]));
					const auto up = glm::normalize(glm::vec3(world[1]));
					ma_engine_listener_set_position(&m_AudioEngine, 0, position.x, position.y, position.z);
					ma_engine_listener_set_direction(&m_AudioEngine, 0, forward.x, forward.y, forward.z);
					ma_engine_listener_set_world_up(&m_AudioEngine, 0, up.x, up.y, up.z);
				}
				if (!data.AudioSource)
				{
					continue;
				}
				Scene::ValidateEntityData(data);
				const auto id = m_Scene.GetPersistentID(entity);
				const auto& component = *data.AudioSource;
				if (!m_AudioSources.contains(id))
				{
					auto source = std::make_unique<AudioRecord>();
					source->Handle = entity;
					source->Path = component.Path;
					const auto path = ResolveAsset(component.Path);
#ifdef _WIN32
					const auto result = ma_sound_init_from_file_w(&m_AudioEngine, path.c_str(), MA_SOUND_FLAG_DECODE,
																  nullptr, nullptr, &source->Sound);
#else
					const auto result = ma_sound_init_from_file(&m_AudioEngine, path.c_str(), MA_SOUND_FLAG_DECODE,
																nullptr, nullptr, &source->Sound);
#endif
					Require(result == MA_SUCCESS,
							"Unable to decode audio " + component.Path + ": " + std::to_string(result));
					source->Initialized = true;
					ma_sound_set_looping(&source->Sound, component.Loop);
					if (component.PlayOnStart)
					{
						Require(ma_sound_start(&source->Sound) == MA_SUCCESS,
								"Unable to start audio: " + component.Path);
					}
					m_AudioSources.emplace(id, std::move(source));
				}
				auto& sound = m_AudioSources.at(id)->Sound;
				ma_sound_set_volume(&sound, component.Volume);
				ma_sound_set_pitch(&sound, component.Pitch);
				ma_sound_set_looping(&sound, component.Loop);
				ma_sound_set_spatialization_enabled(&sound, component.Spatial);
				const auto position = glm::vec3(m_Scene.GetWorldTransform(entity)[3]);
				ma_sound_set_position(&sound, position.x, position.y, position.z);
			}
		}

		AudioRecord& GetAudio(Entity entity)
		{
			Require(m_AudioInitialized, "Audio is disabled");
			Require(m_Scene.Get(entity).AudioSource.has_value(), "Entity requires an audio source");
			SyncAudio();
			return *m_AudioSources.at(m_Scene.GetPersistentID(entity));
		}

		enum class Operation
		{
			Create,
			Destroy,
			Exists,
			Find,
			GetName,
			SetName,
			GetParent,
			SetParent,
			SpawnPrefab,
			GetComponent,
			SetComponent,
			RemoveComponent,
			GetPosition,
			SetPosition,
			ApplyForce,
			ApplyImpulse,
			GetVelocity,
			SetVelocity,
			PlayAudio,
			StopAudio,
			IsAudioPlaying,
			Log,
			KeyDown,
			KeyPressed,
			KeyReleased,
			MouseDown,
			MousePressed,
			MouseReleased,
			MousePosition,
			MouseDelta,
			MouseWheel,
			InputFocused
		};

		void RegisterBindings()
		{
			const std::pair<const char*, Operation> bindings[] = {{"create", Operation::Create},
																  {"destroy", Operation::Destroy},
																  {"exists", Operation::Exists},
																  {"find", Operation::Find},
																  {"get_name", Operation::GetName},
																  {"set_name", Operation::SetName},
																  {"get_parent", Operation::GetParent},
																  {"set_parent", Operation::SetParent},
																  {"spawn_prefab", Operation::SpawnPrefab},
																  {"get_component", Operation::GetComponent},
																  {"set_component", Operation::SetComponent},
																  {"remove_component", Operation::RemoveComponent},
																  {"get_position", Operation::GetPosition},
																  {"set_position", Operation::SetPosition},
																  {"apply_force", Operation::ApplyForce},
																  {"apply_impulse", Operation::ApplyImpulse},
																  {"get_velocity", Operation::GetVelocity},
																  {"set_velocity", Operation::SetVelocity},
																  {"play_audio", Operation::PlayAudio},
																  {"stop_audio", Operation::StopAudio},
																  {"is_audio_playing", Operation::IsAudioPlaying},
																  {"log", Operation::Log},
																  {"key_down", Operation::KeyDown},
																  {"key_pressed", Operation::KeyPressed},
																  {"key_released", Operation::KeyReleased},
																  {"mouse_down", Operation::MouseDown},
																  {"mouse_pressed", Operation::MousePressed},
																  {"mouse_released", Operation::MouseReleased},
																  {"mouse_position", Operation::MousePosition},
																  {"mouse_delta", Operation::MouseDelta},
																  {"mouse_wheel", Operation::MouseWheel},
																  {"input_focused", Operation::InputFocused}};
			lua_newtable(m_State);
			for (const auto& [name, operation] : bindings)
			{
				lua_pushlightuserdata(m_State, this);
				lua_pushinteger(m_State, static_cast<lua_Integer>(operation));
				lua_pushcclosure(
					m_State,
					[](lua_State* state) -> int
					{
						auto* implementation = static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
						const auto selected = static_cast<Operation>(lua_tointeger(state, lua_upvalueindex(2)));
						struct StateScope
						{
							lua_State*& Slot;
							lua_State* Previous;
							~StateScope()
							{
								Slot = Previous;
							}
						} scope{implementation->m_State, implementation->m_State};
						implementation->m_State = state;
						try
						{
							const auto result = implementation->Dispatch(selected);
							return result;
						}
						catch (const std::exception& error)
						{
							lua_pushstring(state, error.what());
						}
						// Lua's C++ unwind preserves StateScope and engine RAII objects.
						return lua_error(state);
					},
					2);
				lua_setfield(m_State, -2, name);
			}
			lua_setglobal(m_State, "engine");
		}

		std::uint64_t ReadID(int index)
		{
			Require(lua_isinteger(m_State, index), "Entity ID must be an integer");
			const auto id = lua_tointeger(m_State, index);
			Require(id > 0, "Entity ID must be positive");
			return static_cast<std::uint64_t>(id);
		}

		Entity ReadEntity(int index)
		{
			const auto entity = m_Scene.FindByID(ReadID(index));
			Require(m_Scene.IsAlive(entity), "Entity is stale or does not exist");
			return entity;
		}

		void PushEntity(Entity entity)
		{
			if (entity)
			{
				lua_pushinteger(m_State, static_cast<lua_Integer>(m_Scene.GetPersistentID(entity)));
			}
			else
			{
				lua_pushnil(m_State);
			}
		}

		glm::vec3 ReadXYZ(int first)
		{
			return {ReadNumber(m_State, first), ReadNumber(m_State, first + 1), ReadNumber(m_State, first + 2)};
		}

		int PushXYZ(glm::vec3 value)
		{
			lua_pushnumber(m_State, value.x);
			lua_pushnumber(m_State, value.y);
			lua_pushnumber(m_State, value.z);
			return 3;
		}

		int Dispatch(Operation operation)
		{
			switch (operation)
			{
			case Operation::Create:
				PushEntity(m_Scene.CreateEntity(ReadString(m_State, 1)));
				return 1;
			case Operation::Destroy:
				m_Scene.DestroyEntity(ReadEntity(1));
				return 0;
			case Operation::Exists:
				lua_pushboolean(m_State, m_Scene.IsAlive(m_Scene.FindByID(ReadID(1))));
				return 1;
			case Operation::Find:
			{
				const auto name = ReadString(m_State, 1);
				for (const auto entity : m_Scene.Entities())
				{
					if (m_Scene.Get(entity).Name == name)
					{
						PushEntity(entity);
						return 1;
					}
				}
				lua_pushnil(m_State);
				return 1;
			}
			case Operation::GetName:
				PushValue(m_State, m_Scene.Get(ReadEntity(1)).Name);
				return 1;
			case Operation::SetName:
			{
				const auto entity = ReadEntity(1);
				auto data = m_Scene.Get(entity);
				data.Name = ReadString(m_State, 2);
				Scene::ValidateEntityData(data);
				m_Scene.Get(entity) = std::move(data);
				return 0;
			}
			case Operation::GetParent:
				PushEntity(m_Scene.GetParent(ReadEntity(1)));
				return 1;
			case Operation::SetParent:
			{
				const auto entity = ReadEntity(1);
				const auto parent = lua_isnoneornil(m_State, 2) ? Entity{} : ReadEntity(2);
				m_Scene.SetParent(entity, parent);
				return 0;
			}
			case Operation::SpawnPrefab:
			{
				const auto path = ResolveAsset(ReadString(m_State, 1));
				const auto parent = lua_isnoneornil(m_State, 2) ? Entity{} : ReadEntity(2);
				const auto prefab = Scene::Load(path);
				Entity root;
				for (const auto entity : prefab.Entities())
				{
					if (!prefab.GetParent(entity))
					{
						Require(!root, "Prefab must have exactly one root entity");
						root = entity;
					}
				}
				Require(static_cast<bool>(root), "Prefab is empty");
				PushEntity(m_Scene.InstantiatePrefab(prefab, root, parent));
				return 1;
			}
			case Operation::GetComponent:
				return GetComponent(ReadEntity(1), ReadString(m_State, 2));
			case Operation::SetComponent:
				SetComponent(ReadEntity(1), ReadString(m_State, 2));
				return 0;
			case Operation::RemoveComponent:
				RemoveComponent(ReadEntity(1), ReadString(m_State, 2));
				return 0;
			case Operation::GetPosition:
				return PushXYZ(m_Scene.Get(ReadEntity(1)).Transform.Translation);
			case Operation::SetPosition:
			{
				const auto entity = ReadEntity(1);
				m_Scene.Get(entity).Transform.Translation = ReadXYZ(2);
				return 0;
			}
			case Operation::ApplyForce:
				ApplyForce(ReadEntity(1), ReadXYZ(2));
				return 0;
			case Operation::ApplyImpulse:
				ApplyImpulse(ReadEntity(1), ReadXYZ(2));
				return 0;
			case Operation::GetVelocity:
				return PushXYZ(GetVelocity(ReadEntity(1)));
			case Operation::SetVelocity:
				SetVelocity(ReadEntity(1), ReadXYZ(2));
				return 0;
			case Operation::PlayAudio:
				PlayAudio(ReadEntity(1));
				return 0;
			case Operation::StopAudio:
				StopAudio(ReadEntity(1));
				return 0;
			case Operation::IsAudioPlaying:
				lua_pushboolean(m_State, IsAudioPlaying(ReadEntity(1)));
				return 1;
			case Operation::Log:
				m_Log.push_back(ReadString(m_State, 1));
				return 0;
			case Operation::KeyDown:
			case Operation::KeyPressed:
			case Operation::KeyReleased:
			{
				const auto key = ReadString(m_State, 1);
				Require(InputState::IsValidKey(key), "Unsupported key: " + key);
				const auto& keys = operation == Operation::KeyDown		? m_CurrentInput.KeysDown
								   : operation == Operation::KeyPressed ? m_CurrentInput.KeysPressed
																		: m_CurrentInput.KeysReleased;
				lua_pushboolean(m_State, keys.contains(key));
				return 1;
			}
			case Operation::MouseDown:
			case Operation::MousePressed:
			case Operation::MouseReleased:
			{
				const auto index = InputState::MouseButtonIndex(ReadString(m_State, 1));
				const auto& buttons = operation == Operation::MouseDown		 ? m_CurrentInput.MouseDown
									  : operation == Operation::MousePressed ? m_CurrentInput.MousePressed
																			 : m_CurrentInput.MouseReleased;
				lua_pushboolean(m_State, buttons[index]);
				return 1;
			}
			case Operation::MousePosition:
			case Operation::MouseDelta:
			case Operation::MouseWheel:
			{
				const auto value = operation == Operation::MousePosition ? m_CurrentInput.MousePosition
								   : operation == Operation::MouseDelta	 ? m_CurrentInput.MouseDelta
																		 : m_CurrentInput.Wheel;
				lua_pushnumber(m_State, value.x);
				lua_pushnumber(m_State, value.y);
				return 2;
			}
			case Operation::InputFocused:
				lua_pushboolean(m_State, m_CurrentInput.Focused);
				return 1;
			}
			throw std::runtime_error("Unknown engine operation");
		}

		int GetComponent(Entity entity, const std::string& name)
		{
			const auto& data = m_Scene.Get(entity);
			if (name == "Transform")
			{
				PushComponent(m_State, data.Transform);
			}
			else if (name == "Camera")
			{
				PushComponent(m_State, data.Camera);
			}
			else if (name == "MeshRenderer")
			{
				PushComponent(m_State, data.MeshRenderer);
			}
			else if (name == "Light")
			{
				PushComponent(m_State, data.Light);
			}
			else if (name == "RigidBody")
			{
				PushComponent(m_State, data.RigidBody);
			}
			else if (name == "Script")
			{
				PushComponent(m_State, data.Script);
			}
			else if (name == "AudioSource")
			{
				PushComponent(m_State, data.AudioSource);
			}
			else
			{
				throw std::runtime_error("Unknown component: " + name);
			}
			return 1;
		}

		void SetComponent(Entity entity, const std::string& name)
		{
			auto data = m_Scene.Get(entity);
			if (name == "Transform")
			{
				data.Transform = ReadComponent<TransformComponent>(m_State, 3);
			}
			else if (name == "Camera")
			{
				data.Camera = ReadComponent<CameraComponent>(m_State, 3);
			}
			else if (name == "MeshRenderer")
			{
				data.MeshRenderer = ReadComponent<MeshRendererComponent>(m_State, 3);
			}
			else if (name == "Light")
			{
				data.Light = ReadComponent<LightComponent>(m_State, 3);
			}
			else if (name == "RigidBody")
			{
				data.RigidBody = ReadComponent<RigidBodyComponent>(m_State, 3);
			}
			else if (name == "Script")
			{
				data.Script = ReadComponent<ScriptComponent>(m_State, 3);
			}
			else if (name == "AudioSource")
			{
				data.AudioSource = ReadComponent<AudioSourceComponent>(m_State, 3);
			}
			else
			{
				throw std::runtime_error("Unknown component: " + name);
			}
			Scene::ValidateEntityData(data);
			m_Scene.Get(entity) = std::move(data);
		}

		void RemoveComponent(Entity entity, const std::string& name)
		{
			auto& data = m_Scene.Get(entity);
			if (name == "Camera")
			{
				data.Camera.reset();
			}
			else if (name == "MeshRenderer")
			{
				data.MeshRenderer.reset();
			}
			else if (name == "Light")
			{
				data.Light.reset();
			}
			else if (name == "RigidBody")
			{
				data.RigidBody.reset();
			}
			else if (name == "Script")
			{
				data.Script.reset();
			}
			else if (name == "AudioSource")
			{
				data.AudioSource.reset();
			}
			else
			{
				throw std::runtime_error("Unknown or mandatory component: " + name);
			}
		}

		Scene& m_Scene;
		std::filesystem::path m_AssetRoot;
		SimulationSettings m_Settings;
		btDefaultCollisionConfiguration m_CollisionConfiguration;
		btCollisionDispatcher m_Dispatcher;
		btDbvtBroadphase m_Broadphase;
		btSequentialImpulseConstraintSolver m_Solver;
		btDiscreteDynamicsWorld m_World;
		std::map<std::uint64_t, BodyRecord> m_Bodies;
		std::map<std::uint64_t, ScriptRecord> m_Scripts;
		std::set<std::pair<std::uint64_t, std::uint64_t>> m_Contacts;
		std::map<std::uint64_t, std::unique_ptr<AudioRecord>> m_AudioSources;
		lua_State* m_State = nullptr;
		size_t m_LuaAllocatedBytes = 0;
		int m_InstructionBudget = 0;
		ma_engine m_AudioEngine{};
		bool m_AudioInitialized = false;
		bool m_Running = false;
		double m_Accumulator = 0.0;
		InputSnapshot m_CurrentInput;
		InputSnapshot m_PendingInput;
		std::vector<std::string> m_Errors;
		std::vector<std::string> m_Log;
	};

	Simulation::Simulation(Scene& scene, std::filesystem::path assetRoot, SimulationSettings settings)
		: m_Impl(std::make_unique<Impl>(scene, std::move(assetRoot), settings))
	{
	}
	Simulation::~Simulation() = default;
	void Simulation::Start()
	{
		m_Impl->Start();
	}
	void Simulation::Stop()
	{
		m_Impl->Stop();
	}
	bool Simulation::IsRunning() const
	{
		return m_Impl->IsRunning();
	}
	void Simulation::Update(double deltaTime)
	{
		m_Impl->Update(deltaTime);
	}
	void Simulation::Step()
	{
		m_Impl->Step();
	}
	void Simulation::SetInput(const InputSnapshot& input)
	{
		m_Impl->SetInput(input);
	}
	void Simulation::Execute(std::string_view source, std::string_view label)
	{
		m_Impl->Execute(source, label);
	}
	const std::vector<std::string>& Simulation::GetErrors() const
	{
		return m_Impl->GetErrors();
	}
	const std::vector<std::string>& Simulation::GetLog() const
	{
		return m_Impl->GetLog();
	}
	void Simulation::ClearErrors()
	{
		m_Impl->ClearErrors();
	}
	void Simulation::ApplyForce(Entity entity, glm::vec3 force)
	{
		m_Impl->ApplyForce(entity, force);
	}
	void Simulation::ApplyImpulse(Entity entity, glm::vec3 impulse)
	{
		m_Impl->ApplyImpulse(entity, impulse);
	}
	void Simulation::SetVelocity(Entity entity, glm::vec3 velocity)
	{
		m_Impl->SetVelocity(entity, velocity);
	}
	glm::vec3 Simulation::GetVelocity(Entity entity)
	{
		return m_Impl->GetVelocity(entity);
	}
	void Simulation::PlayAudio(Entity entity)
	{
		m_Impl->PlayAudio(entity);
	}
	void Simulation::StopAudio(Entity entity)
	{
		m_Impl->StopAudio(entity);
	}
	bool Simulation::IsAudioPlaying(Entity entity)
	{
		return m_Impl->IsAudioPlaying(entity);
	}
	std::vector<float> Simulation::RenderAudio(std::uint32_t frameCount)
	{
		return m_Impl->RenderAudio(frameCount);
	}
} // namespace Aster
