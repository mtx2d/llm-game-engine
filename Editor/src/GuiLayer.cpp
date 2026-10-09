#include <Aster/Assets/AssetImporter.h>
#include <Aster/Core/ExecutablePath.h>
#include <Aster/Editor/CommandProcessor.h>
#include <Aster/Editor/GuiLayer.h>
#include <Aster/Editor/GuiRenderer.h>
#include <Aster/Editor/ViewportCamera.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>

// ImGuizmo requires the ImGui types to be declared first.
#include <ImGuizmo.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Aster
{
	namespace
	{
		using Json = nlohmann::json;

		template <std::size_t Size> void SetText(std::array<char, Size>& buffer, const std::string& text)
		{
			if (text.size() >= Size)
			{
				throw std::runtime_error("Text exceeds the editor input limit");
			}
			std::fill(buffer.begin(), buffer.end(), '\0');
			std::copy(text.begin(), text.end(), buffer.begin());
		}

		bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& path)
		{
			auto current = path.begin();
			for (const auto& part : root)
			{
				if (current == path.end() || *current != part)
				{
					return false;
				}
				++current;
			}
			return true;
		}

		Json DefaultEntity()
		{
			Scene defaults("Defaults");
			const auto entity = defaults.CreateEntity();
			auto& data = defaults.Get(entity);
			data.Camera = CameraComponent{};
			data.MeshRenderer = MeshRendererComponent{};
			data.MeshRenderer->Mesh = "Mesh.gltf";
			data.Light = LightComponent{};
			data.RigidBody = RigidBodyComponent{};
			data.Script = ScriptComponent{};
			data.Script->Path = "Behavior.lua";
			data.AudioSource = AudioSourceComponent{};
			data.AudioSource->Path = "Sound.wav";
			return defaults.Serialize().at("Entities").at(0);
		}

		std::optional<double> IntersectTriangle(glm::dvec3 origin, glm::dvec3 direction, glm::dvec3 first,
												glm::dvec3 second, glm::dvec3 third)
		{
			const auto edge = second - first;
			const auto otherEdge = third - first;
			const auto perpendicular = glm::cross(direction, otherEdge);
			const double determinant = glm::dot(edge, perpendicular);
			const double tolerance = 1e-12 * glm::length(edge) * glm::length(perpendicular);
			if (std::abs(determinant) <= tolerance)
			{
				return std::nullopt;
			}
			const auto offset = origin - first;
			const double u = glm::dot(offset, perpendicular) / determinant;
			if (u < 0.0 || u > 1.0)
			{
				return std::nullopt;
			}
			const auto cross = glm::cross(offset, edge);
			const double v = glm::dot(direction, cross) / determinant;
			if (v < 0.0 || u + v > 1.0)
			{
				return std::nullopt;
			}
			const double distance = glm::dot(otherEdge, cross) / determinant;
			return std::isfinite(distance) && distance >= 0.0 ? std::optional(distance) : std::nullopt;
		}
	} // namespace

	class GuiLayer::Impl
	{
	  public:
		Impl(Renderer& renderer, CommandProcessor& commands, std::filesystem::path projectRoot,
			 const std::optional<std::filesystem::path>& scenePath)
			: m_Renderer(renderer), m_Commands(commands), m_ProjectRoot(std::filesystem::canonical(projectRoot)),
			  m_Defaults(DefaultEntity()), m_GuiRenderer(std::make_unique<GuiRenderer>())
		{
			if (!renderer.GetNativeWindow())
			{
				throw std::runtime_error("The graphical editor requires a window");
			}
			SetText(m_ScenePath, scenePath ? scenePath->generic_string() : "NewScene.aster");
			const auto executableDirectory = GetExecutablePath().parent_path();
#ifdef _WIN32
			SetText(m_RuntimePath, (executableDirectory / "AsterRuntime.exe").string());
#else
			SetText(m_RuntimePath, (executableDirectory / "AsterRuntime").string());
#endif
			SetText(m_NoticesPath, (m_ProjectRoot.parent_path() / "ThirdParty").string());
			SetText(m_ExportPath, (m_ProjectRoot.parent_path() / "AsterExport").string());
			IMGUI_CHECKVERSION();
			m_Context = ImGui::CreateContext();
			try
			{
				auto& io = ImGui::GetIO();
				io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
				io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
				io.IniFilename = nullptr;
				ImGui::StyleColorsDark();
				auto& style = ImGui::GetStyle();
				style.WindowRounding = 3;
				style.FrameRounding = 3;
				style.WindowPadding = {12, 12};
				style.Colors[ImGuiCol_WindowBg] = {0.065f, 0.075f, 0.095f, 0.97f};
				style.Colors[ImGuiCol_Header] = {0.15f, 0.25f, 0.36f, 1};
				style.Colors[ImGuiCol_Button] = {0.17f, 0.26f, 0.37f, 1};
				style.Colors[ImGuiCol_ButtonHovered] = {0.23f, 0.38f, 0.54f, 1};
				if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(renderer.GetNativeWindow()), true))
				{
					throw std::runtime_error("Could not initialize editor input");
				}
				m_BackendInitialized = true;
				unsigned char* pixels = nullptr;
				int width = 0;
				int height = 0;
				io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
				io.Fonts->SetTexID(1);
				renderer.SetOverlayCallback(
					[this](nvrhi::IDevice* device, nvrhi::ICommandList* list, nvrhi::IFramebuffer* target)
					{
						ImGui::SetCurrentContext(m_Context);
						m_Device = device;
						m_GuiRenderer->Draw(device, list, target, ImGui::GetDrawData());
					});
				if (scenePath)
				{
					Execute({{"command", "scene.load"}, {"path", scenePath->generic_string()}});
					for (const auto entity : m_Commands.GetScene().Entities())
					{
						if (!m_Selected || m_Commands.GetScene().Get(entity).MeshRenderer)
						{
							m_Selected = m_Commands.GetScene().GetPersistentID(entity);
						}
						if (m_Commands.GetScene().Get(entity).MeshRenderer)
						{
							break;
						}
					}
				}
			}
			catch (...)
			{
				renderer.SetOverlayCallback({});
				if (m_BackendInitialized)
				{
					ImGui_ImplGlfw_Shutdown();
				}
				ImGui::DestroyContext(m_Context);
				throw;
			}
		}

		~Impl()
		{
			m_Renderer.SetOverlayCallback({});
			if (m_Device)
			{
				m_Device->waitForIdle();
			}
			m_GuiRenderer.reset();
			ImGui::SetCurrentContext(m_Context);
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext(m_Context);
		}

		void RunFrame()
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			ImGuizmo::BeginFrame();
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				m_CancelGizmoUntilRelease = false;
				// A gizmo may emit its final delta while processing the release frame.
				// Commit it only after Manipulate retires the active handle below.
				if (m_EditKind != EditKind::Gizmo)
				{
					FinishEdit(true);
				}
			}
			if (m_EditKind != EditKind::None && ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				m_CancelGizmoUntilRelease = m_EditKind == EditKind::Gizmo;
				if (m_CancelGizmoUntilRelease)
				{
					ImGuizmo::Enable(false);
				}
				FinishEdit(false);
			}
			if (m_Selected && !m_Commands.GetScene().FindByID(m_Selected))
			{
				m_Selected = 0;
			}
			if (m_Commands.IsPlaying())
			{
				try
				{
					auto input = m_Renderer.GetInputSnapshot();
					const auto& io = ImGui::GetIO();
					if (io.WantCaptureKeyboard || io.WantTextInput)
					{
						input.KeysReleased.insert(input.KeysDown.begin(), input.KeysDown.end());
						input.KeysDown.clear();
						input.KeysPressed.clear();
					}
					if (io.WantCaptureMouse)
					{
						for (std::size_t index = 0; index < input.MouseDown.size(); ++index)
						{
							input.MouseReleased[index] = input.MouseReleased[index] || input.MouseDown[index];
						}
						input.MouseDown.fill(false);
						input.MousePressed.fill(false);
						input.MouseDelta = {0, 0};
						input.Wheel = {0, 0};
					}
					m_Commands.SetInput(input);
					m_Commands.UpdateSimulation(ImGui::GetIO().DeltaTime);
					if (!m_Commands.GetSimulationErrors().empty())
					{
						m_Status = m_Commands.GetSimulationErrors().back();
					}
				}
				catch (const std::exception& error)
				{
					m_Status = error.what();
				}
			}
			UpdateCamera();
			DrawToolbar();
			DrawHierarchy();
			DrawInspector();
			DrawAssets();
			DrawGizmo();
			PickViewport();
			DrawExport();
			DrawStatus();
			ImGui::Render();
		}

		RenderSettings GetRenderSettings() const
		{
			RenderSettings settings;
			settings.Exposure = m_Exposure;
			if (m_Commands.IsPlaying())
			{
				const auto [view, projection] = m_Renderer.GetSceneCamera(m_Commands.GetScene());
				settings.CameraOverride = RenderCamera{view, FitPerspectiveToViewport(projection, GetViewport())};
			}
			else
			{
				settings.CameraOverride = EditorCamera();
			}
			return settings;
		}

		void SetStatus(std::string status)
		{
			m_Status = std::move(status);
		}

	  private:
		Json Execute(Json request)
		{
			const auto result = m_Commands.Execute(request);
			if (!result.value("ok", false))
			{
				m_Status = result.value("error", "The action failed");
			}
			else if (result.contains("result") && result.at("result").is_object() &&
					 result.at("result").contains("errors") && !result.at("result").at("errors").empty())
			{
				m_Status = result.at("result").at("errors").back().get<std::string>();
			}
			else
			{
				m_Status = "Ready";
			}
			return result;
		}

		enum class EditKind
		{
			None,
			Inspector,
			Gizmo
		};

		void BeginEdit(EditKind kind)
		{
			if (m_EditKind == kind)
			{
				return;
			}
			FinishEdit(true);
			if (Execute({{"command", "history.begin"}}).value("ok", false))
			{
				m_EditKind = kind;
			}
		}

		void FinishEdit(bool commit)
		{
			if (m_EditKind == EditKind::None)
			{
				return;
			}
			m_EditKind = EditKind::None;
			Execute({{"command", commit ? "history.commit" : "history.cancel"}});
		}

		void Patch(const Json& patch)
		{
			Execute({{"command", "entity.patch"}, {"entity", m_Selected}, {"patch", patch}});
		}

		void CreateEntity(const std::string& name)
		{
			const auto result = Execute({{"command", "entity.create"}, {"name", name}});
			if (result.value("ok", false))
			{
				m_Selected = result.at("result").at("entity").get<std::uint64_t>();
			}
		}

		void DrawToolbar()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({0, 0});
			ImGui::SetNextWindowSize({io.DisplaySize.x, 74});
			ImGui::Begin("Aster toolbar", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
			ImGui::TextUnformatted("ASTER");
			ImGui::SameLine(90);
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			if (ImGui::Button("New"))
			{
				Execute({{"command", "scene.new"}, {"name", "Untitled"}});
				m_Selected = 0;
				SetText(m_ScenePath, "NewScene.aster");
			}
			ImGui::SameLine();
			if (ImGui::Button("Undo"))
			{
				Execute({{"command", "history.undo"}});
			}
			ImGui::SameLine();
			if (ImGui::Button("Redo"))
			{
				Execute({{"command", "history.redo"}});
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(m_Commands.IsPlaying() ? "Stop" : "Play"))
			{
				Execute({{"command", m_Commands.IsPlaying() ? "simulation.stop" : "simulation.start"}});
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			if (ImGui::Button("Export game"))
			{
				m_ShowExport = true;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(280);
			ImGui::InputText("##scene-path", m_ScenePath.data(), m_ScenePath.size());
			ImGui::SameLine();
			if (ImGui::Button("Load"))
			{
				Execute({{"command", "scene.load"}, {"path", m_ScenePath.data()}});
				m_Selected = 0;
			}
			ImGui::SameLine();
			if (ImGui::Button("Save"))
			{
				Save();
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("Right mouse + WASD: fly   Q/E: descend/ascend   Shift: faster   Mouse wheel: speed");
			ImGui::SameLine();
			ImGui::Text(" | %s", m_Commands.GetScene().GetName().c_str());
			ImGui::End();
			if (!io.WantTextInput && io.KeyCtrl && !m_Commands.IsPlaying())
			{
				if (ImGui::IsKeyPressed(ImGuiKey_S))
				{
					Save();
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Z))
				{
					Execute({{"command", "history.undo"}});
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Y))
				{
					Execute({{"command", "history.redo"}});
				}
			}
		}

		void Save()
		{
			Execute({{"command", "scene.save"}, {"path", m_ScenePath.data()}});
		}

		void AcceptParentDrop(std::uint64_t parent)
		{
			if (ImGui::BeginDragDropTarget())
			{
				if (const auto* payload = ImGui::AcceptDragDropPayload("ASTER_ENTITY"))
				{
					if (payload->DataSize == sizeof(std::uint64_t))
					{
						const auto child = *static_cast<const std::uint64_t*>(payload->Data);
						Execute({{"command", "entity.parent"}, {"entity", child}, {"parent", parent}});
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		void DrawHierarchy()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({0, 74});
			ImGui::SetNextWindowSize({260, std::max(140.0f, io.DisplaySize.y - 290)});
			ImGui::Begin("Hierarchy", nullptr,
						 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			if (ImGui::Button("Create entity"))
			{
				CreateEntity("Entity");
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(m_Selected == 0);
			if (ImGui::Button("Delete"))
			{
				Execute({{"command", "entity.destroy"}, {"entity", m_Selected}});
				m_Selected = 0;
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			const auto snapshot = m_Commands.GetScene().Serialize();
			std::map<std::uint64_t, std::vector<Json>> children;
			for (const auto& entity : snapshot.at("Entities"))
			{
				children[entity.at("Parent").is_null() ? 0 : entity.at("Parent").get<std::uint64_t>()].push_back(
					entity);
			}
			ImGui::Selectable("Scene root", false);
			if (!m_Commands.IsPlaying())
			{
				AcceptParentDrop(0);
			}
			std::function<void(std::uint64_t, std::size_t)> draw = [&](std::uint64_t parent, std::size_t depth)
			{
				if (depth > 128)
				{
					ImGui::TextDisabled("Hierarchy depth exceeds editor display limit");
					return;
				}
				for (const auto& entity : children[parent])
				{
					const auto id = entity.at("ID").get<std::uint64_t>();
					ImGui::PushID(std::to_string(id).c_str());
					ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
					if (id == m_Selected)
					{
						flags |= ImGuiTreeNodeFlags_Selected;
					}
					if (children[id].empty())
					{
						flags |= ImGuiTreeNodeFlags_Leaf;
					}
					const bool open =
						ImGui::TreeNodeEx("entity", flags, "%s", entity.at("Name").get<std::string>().c_str());
					if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
					{
						m_Selected = id;
					}
					if (!m_Commands.IsPlaying())
					{
						if (ImGui::BeginDragDropSource())
						{
							ImGui::SetDragDropPayload("ASTER_ENTITY", &id, sizeof(id));
							ImGui::TextUnformatted(entity.at("Name").get<std::string>().c_str());
							ImGui::EndDragDropSource();
						}
						AcceptParentDrop(id);
					}
					if (open)
					{
						draw(id, depth + 1);
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
			};
			draw(0, 0);
			ImGui::End();
		}

		bool EditString(const char* label, const std::string& key, Json& value)
		{
			if (value.get_ref<const std::string&>().size() >= 4096)
			{
				ImGui::TextWrapped("%s: text exceeds the editor input limit", label);
				return false;
			}
			auto [entry, inserted] = m_TextBuffers.try_emplace(key);
			if (inserted)
			{
				SetText(entry->second, value.get<std::string>());
			}
			const bool enter = ImGui::InputText(label, entry->second.data(), entry->second.size(),
												ImGuiInputTextFlags_EnterReturnsTrue);
			const bool changed = enter || ImGui::IsItemDeactivatedAfterEdit();
			if (changed)
			{
				value = entry->second.data();
			}
			else if (!ImGui::IsItemActive())
			{
				SetText(entry->second, value.get<std::string>());
			}
			return changed;
		}

		bool EditField(const std::string& component, const std::string& name, Json& value)
		{
			const std::string key = std::to_string(m_Selected) + "/" + component + "/" + name;
			if (value.is_boolean())
			{
				bool boolean = value.get<bool>();
				if (ImGui::Checkbox(name.c_str(), &boolean))
				{
					value = boolean;
					return true;
				}
			}
			else if (value.is_number())
			{
				float number = value.get<float>();
				if (ImGui::DragFloat(name.c_str(), &number, name == "Mass" ? 0.1f : 0.02f))
				{
					value = number;
					return true;
				}
			}
			else if (value.is_array() && (value.size() == 3 || value.size() == 4))
			{
				float vector[4] = {};
				for (std::size_t index = 0; index < value.size(); ++index)
				{
					vector[index] = value[index].get<float>();
				}
				bool edited = false;
				if (name == "BaseColor")
				{
					edited = ImGui::ColorEdit4(name.c_str(), vector);
				}
				else if (name == "Color")
				{
					edited = ImGui::ColorEdit3(name.c_str(), vector);
				}
				else
				{
					edited = ImGui::DragFloat3(name.c_str(), vector, 0.02f);
				}
				if (edited)
				{
					for (std::size_t index = 0; index < value.size(); ++index)
					{
						value[index] = vector[index];
					}
					return true;
				}
			}
			else if (value.is_string())
			{
				std::vector<const char*> options;
				if (component == "RigidBody" && name == "Type")
				{
					options = {"Static", "Kinematic", "Dynamic"};
				}
				if (component == "RigidBody" && name == "Shape")
				{
					options = {"Box", "Sphere", "Capsule"};
				}
				if (component == "Light" && name == "Type")
				{
					options = {"Directional", "Point", "Spot"};
				}
				if (options.empty())
				{
					return EditString(name.c_str(), key, value);
				}
				if (ImGui::BeginCombo(name.c_str(), value.get<std::string>().c_str()))
				{
					bool changed = false;
					for (const auto* option : options)
					{
						if (ImGui::Selectable(option, value == option))
						{
							value = option;
							changed = true;
						}
					}
					ImGui::EndCombo();
					return changed;
				}
			}
			return false;
		}

		void DrawInspector()
		{
			if (m_CachedSelected != m_Selected)
			{
				m_TextBuffers.clear();
				m_CachedSelected = m_Selected;
			}
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({std::max(260.0f, io.DisplaySize.x - 330), 74});
			ImGui::SetNextWindowSize({330, std::max(140.0f, io.DisplaySize.y - 102)});
			ImGui::Begin("Inspector", nullptr,
						 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::SetNextItemWidth(140);
			ImGui::SliderFloat("Exposure", &m_Exposure, 0.05f, 5.0f);
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			auto environment = m_Commands.GetScene().GetEnvironment();
			ImGui::TextWrapped("Environment: %s", environment.Path.empty() ? "None (open an HDR asset to add one)"
																		   : environment.Path.c_str());
			if (!environment.Path.empty())
			{
				if (ImGui::DragFloat("Sky intensity", &environment.Intensity, 0.01f, 0.0f, 100.0f))
				{
					if (ImGui::IsItemActive())
					{
						BeginEdit(EditKind::Inspector);
					}
					Execute(
						{{"command", "scene.environment"}, {"environment", {{"Intensity", environment.Intensity}}}});
				}
				if (ImGui::DragFloat("Sky rotation (radians)", &environment.Rotation, 0.01f))
				{
					if (ImGui::IsItemActive())
					{
						BeginEdit(EditKind::Inspector);
					}
					Execute({{"command", "scene.environment"}, {"environment", {{"Rotation", environment.Rotation}}}});
				}
				if (ImGui::SmallButton("Remove environment"))
				{
					Execute({{"command", "scene.environment"}, {"environment", {{"Path", ""}}}});
				}
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			Json selected;
			const auto snapshot = m_Commands.GetScene().Serialize();
			for (const auto& entity : snapshot.at("Entities"))
			{
				if (entity.at("ID") == m_Selected)
				{
					selected = entity;
					break;
				}
			}
			if (selected.is_null())
			{
				ImGui::TextWrapped("Select an entity in the hierarchy to edit its components.");
				ImGui::End();
				return;
			}
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			if (EditField("Entity", "Name", selected["Name"]))
			{
				Patch({{"Name", selected["Name"]}});
			}
			if (ImGui::Button("Add component"))
			{
				ImGui::OpenPopup("Components");
			}
			if (ImGui::BeginPopup("Components"))
			{
				for (const auto* component : {"Camera", "Light", "RigidBody"})
				{
					if (!selected.contains(component) && ImGui::MenuItem(component))
					{
						Patch({{component, m_Defaults.at(component)}});
					}
				}
				ImGui::Separator();
				ImGui::TextDisabled("Attach meshes, scripts, and audio from Assets.");
				ImGui::EndPopup();
			}
			for (const auto* component :
				 {"Transform", "Camera", "MeshRenderer", "Light", "RigidBody", "Script", "AudioSource"})
			{
				if (!selected.contains(component))
				{
					continue;
				}
				ImGui::PushID(component);
				if (ImGui::CollapsingHeader(component, ImGuiTreeNodeFlags_DefaultOpen))
				{
					if (std::string(component) != "Transform" && ImGui::SmallButton("Remove"))
					{
						Patch({{component, nullptr}});
						ImGui::PopID();
						continue;
					}
					for (auto& [name, value] : selected[component].items())
					{
						ImGui::SetNextItemWidth(165);
						if (EditField(component, name, value))
						{
							if ((value.is_number() || value.is_array()) && ImGui::IsItemActive())
							{
								BeginEdit(EditKind::Inspector);
							}
							Patch({{component, {{name, value}}}});
						}
					}
				}
				ImGui::PopID();
			}
			ImGui::EndDisabled();
			ImGui::End();
		}

		void ActivateAsset(const std::filesystem::path& relative)
		{
			const auto extension = relative.extension().string();
			if (extension == ".hdr")
			{
				Execute({{"command", "scene.environment"}, {"environment", {{"Path", relative.generic_string()}}}});
				return;
			}
			if (extension == ".aster")
			{
				if (Execute({{"command", "scene.load"}, {"path", relative.generic_string()}}).value("ok", false))
				{
					SetText(m_ScenePath, relative.generic_string());
					m_Selected = 0;
				}
				return;
			}
			if (extension == ".json")
			{
				const auto prefab = Scene::Load(m_ProjectRoot / relative);
				for (const auto entity : prefab.Entities())
				{
					if (!prefab.GetParent(entity))
					{
						const auto result = Execute({{"command", "prefab.spawn"},
													 {"path", relative.generic_string()},
													 {"root", prefab.GetPersistentID(entity)}});
						if (result.value("ok", false))
						{
							m_Selected = result.at("result").at("entity").get<std::uint64_t>();
						}
						return;
					}
				}
				throw std::runtime_error("Prefab has no root entity");
			}
			std::string component;
			std::string field;
			if (extension == ".gltf" || extension == ".glb")
			{
				component = "MeshRenderer";
				field = "Mesh";
			}
			else if (extension == ".lua")
			{
				component = "Script";
				field = "Path";
			}
			else if (extension == ".wav" || extension == ".mp3" || extension == ".flac")
			{
				component = "AudioSource";
				field = "Path";
			}
			else
			{
				throw std::runtime_error("This file cannot be attached to an entity");
			}
			if (!m_Selected)
			{
				CreateEntity(relative.stem().string());
			}
			if (!m_Selected)
			{
				return;
			}
			Json value = m_Defaults.at(component);
			const auto snapshot = m_Commands.GetScene().Serialize();
			for (const auto& entity : snapshot.at("Entities"))
			{
				if (entity.at("ID") == m_Selected && entity.contains(component))
				{
					value = entity.at(component);
				}
			}
			value[field] = relative.generic_string();
			Patch({{component, value}});
		}

		void DrawAssets()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({0, std::max(214.0f, io.DisplaySize.y - 216)});
			ImGui::SetNextWindowSize({std::max(260.0f, io.DisplaySize.x - 330), 188});
			ImGui::Begin("Assets", nullptr,
						 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			if (!m_AssetDirectory.empty() && ImGui::SmallButton("Up"))
			{
				m_AssetDirectory = m_AssetDirectory.parent_path();
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(m_AssetDirectory.empty() ? "Project assets"
															: m_AssetDirectory.generic_string().c_str());
			ImGui::TextDisabled("Double-click a scene to open it, or an asset to attach it to the selected entity.");
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			try
			{
				const auto directory = std::filesystem::canonical(m_ProjectRoot / m_AssetDirectory);
				if (!IsWithin(m_ProjectRoot, directory))
				{
					throw std::runtime_error("Asset folder escapes the project");
				}
				std::vector<std::filesystem::directory_entry> entries;
				for (const auto& entry : std::filesystem::directory_iterator(directory))
				{
					entries.push_back(entry);
				}
				std::sort(entries.begin(), entries.end(),
						  [](const auto& first, const auto& second) { return first.path() < second.path(); });
				std::optional<std::filesystem::path> openDirectory;
				for (const auto& entry : entries)
				{
					const auto canonical = std::filesystem::canonical(entry.path());
					if (!IsWithin(m_ProjectRoot, canonical))
					{
						continue;
					}
					const auto relative = m_AssetDirectory / entry.path().filename();
					const auto label = (entry.is_directory() ? "[Folder] " : "") + entry.path().filename().string();
					ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						if (entry.is_directory())
						{
							openDirectory = relative;
						}
						else
						{
							ActivateAsset(relative);
						}
					}
				}
				if (openDirectory)
				{
					m_AssetDirectory = *openDirectory;
				}
			}
			catch (const std::exception& error)
			{
				m_Status = error.what();
			}
			ImGui::EndDisabled();
			ImGui::End();
		}

		EditorViewport GetViewport() const
		{
			const auto size = ImGui::GetIO().DisplaySize;
			return CalculateEditorViewport({size.x, size.y});
		}

		bool IsMouseInViewport() const
		{
			const auto viewport = GetViewport();
			const auto mouse = ImGui::GetIO().MousePos;
			return mouse.x >= viewport.Position.x && mouse.y >= viewport.Position.y &&
				   mouse.x < viewport.Position.x + viewport.Size.x && mouse.y < viewport.Position.y + viewport.Size.y;
		}

		RenderCamera EditorCamera() const
		{
			const glm::vec3 forward{std::cos(m_Pitch) * std::sin(m_Yaw), std::sin(m_Pitch),
									-std::cos(m_Pitch) * std::cos(m_Yaw)};
			const auto view = glm::lookAtRH(m_CameraPosition, m_CameraPosition + forward, glm::vec3(0, 1, 0));
			const auto projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.05f, 2000.0f);
			return {view, FitPerspectiveToViewport(projection, GetViewport())};
		}

		void UpdateCamera()
		{
			const auto& io = ImGui::GetIO();
			if (m_Commands.IsPlaying() || io.WantCaptureMouse || !IsMouseInViewport() || ImGuizmo::IsUsing())
			{
				return;
			}
			m_CameraSpeed = std::clamp(m_CameraSpeed * std::pow(1.2f, io.MouseWheel), 0.1f, 200.0f);
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
			{
				return;
			}
			m_Yaw += io.MouseDelta.x * 0.004f;
			m_Pitch = std::clamp(m_Pitch - io.MouseDelta.y * 0.004f, -1.5f, 1.5f);
			const glm::vec3 forward{std::cos(m_Pitch) * std::sin(m_Yaw), std::sin(m_Pitch),
									-std::cos(m_Pitch) * std::cos(m_Yaw)};
			const auto right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
			glm::vec3 motion(0);
			if (ImGui::IsKeyDown(ImGuiKey_W))
			{
				motion += forward;
			}
			if (ImGui::IsKeyDown(ImGuiKey_S))
			{
				motion -= forward;
			}
			if (ImGui::IsKeyDown(ImGuiKey_D))
			{
				motion += right;
			}
			if (ImGui::IsKeyDown(ImGuiKey_A))
			{
				motion -= right;
			}
			if (ImGui::IsKeyDown(ImGuiKey_E))
			{
				motion.y += 1;
			}
			if (ImGui::IsKeyDown(ImGuiKey_Q))
			{
				motion.y -= 1;
			}
			if (glm::length(motion) > 0)
			{
				m_CameraPosition +=
					glm::normalize(motion) * m_CameraSpeed * std::min(io.DeltaTime, 0.1f) * (io.KeyShift ? 3.0f : 1.0f);
			}
		}

		void DrawGizmo()
		{
			if (m_Commands.IsPlaying())
			{
				return;
			}
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({272, 84});
			ImGui::SetNextWindowBgAlpha(0.85f);
			ImGui::Begin("Transform tools", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
			if (ImGui::RadioButton("Move", m_GizmoOperation == ImGuizmo::TRANSLATE))
			{
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			}
			ImGui::SameLine();
			if (ImGui::RadioButton("Rotate", m_GizmoOperation == ImGuizmo::ROTATE))
			{
				m_GizmoOperation = ImGuizmo::ROTATE;
			}
			ImGui::SameLine();
			if (ImGui::RadioButton("Scale", m_GizmoOperation == ImGuizmo::SCALE))
			{
				m_GizmoOperation = ImGuizmo::SCALE;
			}
			ImGui::SameLine();
			ImGui::Checkbox("Local", &m_LocalGizmo);
			ImGui::End();
			const auto entity = m_Commands.GetScene().FindByID(m_Selected);
			if (!entity || m_CancelGizmoUntilRelease)
			{
				return;
			}
			auto world = m_Commands.GetScene().GetWorldTransform(entity);
			auto camera = EditorCamera();
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
			ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
			// ImGuizmo requests WantCaptureMouse while merely hovering a handle.
			// Only actual UI windows/widgets block a new drag, not its own capture request.
			const bool hoveredPanel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
			const bool popupOpen =
				ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
			ImGuizmo::Enable((!popupOpen && !hoveredPanel && !ImGui::IsAnyItemActive() && IsMouseInViewport()) ||
							 ImGuizmo::IsUsing());
			if (ImGuizmo::Manipulate(glm::value_ptr(camera.View), glm::value_ptr(camera.Projection), m_GizmoOperation,
									 m_LocalGizmo ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(world)))
			{
				BeginEdit(EditKind::Gizmo);
				const auto parent = m_Commands.GetScene().GetParent(entity);
				if (parent)
				{
					world = glm::inverse(m_Commands.GetScene().GetWorldTransform(parent)) * world;
				}
				glm::vec3 scale, position, skew;
				glm::quat rotation;
				glm::vec4 perspective;
				if (!glm::decompose(world, scale, rotation, position, skew, perspective) || glm::length(skew) > 0.001f)
				{
					m_Status = "This transform would introduce unsupported shear";
					return;
				}
				const auto angles = glm::eulerAngles(rotation);
				Patch({{"Transform",
						{{"Translation", {position.x, position.y, position.z}},
						 {"Rotation", {angles.x, angles.y, angles.z}},
						 {"Scale", {scale.x, scale.y, scale.z}}}}});
			}
			if (m_EditKind == EditKind::Gizmo && !ImGuizmo::IsUsing())
			{
				FinishEdit(true);
			}
		}

		void PickViewport()
		{
			if (m_Commands.IsPlaying() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || !IsMouseInViewport() ||
				ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive() ||
				ImGuizmo::IsUsing() ||
				ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			{
				return;
			}
			if (m_Selected && m_Commands.GetScene().FindByID(m_Selected) && ImGuizmo::IsOver(m_GizmoOperation))
			{
				return;
			}
			try
			{
				const auto camera = EditorCamera();
				const auto inverse = glm::inverse(glm::dmat4(camera.Projection) * glm::dmat4(camera.View));
				const auto& io = ImGui::GetIO();
				const double x = 2.0 * io.MousePos.x / io.DisplaySize.x - 1.0;
				const double y = 1.0 - 2.0 * io.MousePos.y / io.DisplaySize.y;
				const auto nearClip = inverse * glm::dvec4(x, y, 0.0, 1.0);
				const auto farClip = inverse * glm::dvec4(x, y, 1.0, 1.0);
				const auto origin = glm::dvec3(nearClip) / nearClip.w;
				const auto target = glm::dvec3(farClip) / farClip.w;
				const auto direction = glm::normalize(target - origin);
				double closest = glm::length(target - origin);
				std::uint64_t selected = 0;
				AssetImporter importer(m_ProjectRoot);
				// Import once per distinct path for this click. Keeping the cache local
				// avoids stale selection geometry after meshes or dependencies are edited.
				std::map<std::string, MeshAsset> meshes;
				for (const auto entity : m_Commands.GetScene().Entities())
				{
					const auto& data = m_Commands.GetScene().Get(entity);
					if (!data.MeshRenderer || !data.MeshRenderer->Visible)
					{
						continue;
					}
					const auto& path = data.MeshRenderer->Mesh;
					auto [mesh, inserted] = meshes.try_emplace(path);
					if (inserted)
					{
						mesh->second = importer.LoadMesh(path);
					}
					const auto world = glm::dmat4(m_Commands.GetScene().GetWorldTransform(entity));
					for (const auto& primitive : mesh->second.Primitives)
					{
						const auto transform = world * glm::dmat4(primitive.Transform);
						for (std::size_t index = 0; index + 2 < primitive.Indices.size(); index += 3)
						{
							std::array<glm::dvec3, 3> triangle;
							for (std::size_t vertex = 0; vertex < triangle.size(); ++vertex)
							{
								const auto position = primitive.Vertices[primitive.Indices[index + vertex]].Position;
								triangle[vertex] = glm::dvec3(transform * glm::dvec4(position, 1.0));
							}
							const auto distance =
								IntersectTriangle(origin, direction, triangle[0], triangle[1], triangle[2]);
							if (distance && *distance < closest)
							{
								closest = *distance;
								selected = m_Commands.GetScene().GetPersistentID(entity);
							}
						}
					}
				}
				m_Selected = selected;
			}
			catch (const std::exception& error)
			{
				m_Status = "Could not select a mesh: " + std::string(error.what());
			}
		}

		void DrawExport()
		{
			if (m_ShowExport)
			{
				ImGui::OpenPopup("Export game");
				m_ShowExport = false;
			}
			if (ImGui::BeginPopupModal("Export game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextWrapped("Save your scene, then export the runtime and all project assets.");
				ImGui::InputText("Scene", m_ScenePath.data(), m_ScenePath.size());
				ImGui::InputText("Destination", m_ExportPath.data(), m_ExportPath.size());
				ImGui::InputText("Runtime executable", m_RuntimePath.data(), m_RuntimePath.size());
				ImGui::InputText("License notices", m_NoticesPath.data(), m_NoticesPath.size());
				if (ImGui::Button("Export"))
				{
					const auto saved = Execute({{"command", "scene.save"}, {"path", m_ScenePath.data()}});
					if (saved.value("ok", false))
					{
						const auto exported = Execute({{"command", "project.export"},
													   {"scene", m_ScenePath.data()},
													   {"runtime", m_RuntimePath.data()},
													   {"notices", m_NoticesPath.data()},
													   {"output", m_ExportPath.data()}});
						if (exported.value("ok", false))
						{
							m_Status = "Game exported to " + std::string(m_ExportPath.data());
							ImGui::CloseCurrentPopup();
						}
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					ImGui::CloseCurrentPopup();
				}
				ImGui::TextWrapped("%s", m_Status.c_str());
				ImGui::EndPopup();
			}
		}

		void DrawStatus()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos({0, io.DisplaySize.y - 28});
			ImGui::SetNextWindowSize({io.DisplaySize.x, 28});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 5});
			ImGui::Begin("Status", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
							 ImGuiWindowFlags_NoScrollbar);
			ImGui::TextUnformatted(m_Status.c_str());
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", m_Status.c_str());
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}

		Renderer& m_Renderer;
		CommandProcessor& m_Commands;
		std::filesystem::path m_ProjectRoot;
		std::filesystem::path m_AssetDirectory;
		Json m_Defaults;
		std::unique_ptr<GuiRenderer> m_GuiRenderer;
		nvrhi::DeviceHandle m_Device;
		ImGuiContext* m_Context = nullptr;
		bool m_BackendInitialized = false;
		std::uint64_t m_Selected = 0;
		std::uint64_t m_CachedSelected = 0;
		std::array<char, 4096> m_ScenePath{};
		std::array<char, 4096> m_RuntimePath{};
		std::array<char, 4096> m_NoticesPath{};
		std::array<char, 4096> m_ExportPath{};
		std::map<std::string, std::array<char, 4096>> m_TextBuffers;
		std::string m_Status = "Ready";
		glm::vec3 m_CameraPosition{7, 5, 10};
		float m_Yaw = -0.61f;
		float m_Pitch = -0.35f;
		float m_CameraSpeed = 5;
		float m_Exposure = 1;
		ImGuizmo::OPERATION m_GizmoOperation = ImGuizmo::TRANSLATE;
		bool m_LocalGizmo = false;
		bool m_ShowExport = false;
		EditKind m_EditKind = EditKind::None;
		bool m_CancelGizmoUntilRelease = false;
	};

	GuiLayer::GuiLayer(Renderer& renderer, CommandProcessor& commands, std::filesystem::path projectRoot,
					   std::optional<std::filesystem::path> scenePath)
		: m_Impl(std::make_unique<Impl>(renderer, commands, std::move(projectRoot), scenePath))
	{
	}
	GuiLayer::~GuiLayer() = default;
	void GuiLayer::RunFrame()
	{
		m_Impl->RunFrame();
	}
	RenderSettings GuiLayer::GetRenderSettings() const
	{
		return m_Impl->GetRenderSettings();
	}
	void GuiLayer::SetStatus(std::string status)
	{
		m_Impl->SetStatus(std::move(status));
	}
} // namespace Aster
