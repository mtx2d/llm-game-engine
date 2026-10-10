#include <Aster/Assets/AssetImporter.h>
#include <Aster/Assets/AssetPath.h>
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
#include <chrono>
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
			SetText(m_NewProjectName, "My Game");
			if (m_Commands.GetStartupError())
			{
				RefreshProjectFields();
				m_ShowProjects = true;
				m_Status = *m_Commands.GetStartupError();
			}
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
					const auto loaded = Execute({{"command", "scene.load"}, {"path", scenePath->generic_string()}});
					if (!loaded.value("ok", false))
					{
						throw std::runtime_error(loaded.at("error").get<std::string>());
					}
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
			if (m_Renderer.ShouldClose())
			{
				// Include this frame's final gizmo/inspector delta before deciding
				// whether close needs confirmation, then defer the native request.
				m_Renderer.CancelCloseRequest();
				RequestClose();
			}
			DrawExport();
			DrawProjects();
			DrawProjectLauncher();
			DrawProjectBrowser();
			DrawDocumentChange();
			if (const auto warning = m_Commands.UpdateRecovery())
			{
				m_Status = *warning;
			}
			DrawRecovery();
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

		void ShowProjectLauncher()
		{
			const auto response = m_Commands.Execute({{"command", "project.recent"}});
			m_RecentProjects = Json::array();
			m_LauncherError.clear();
			if (response.value("ok", false))
			{
				m_RecentProjects = response.at("result").at("projects");
				if (!response.at("result").at("enabled").get<bool>())
				{
					m_LauncherError = "Recent-project history is unavailable for this session.";
				}
			}
			else
			{
				m_LauncherError = response.at("error").get<std::string>();
			}
			m_ShowLauncher = true;
		}

	  private:
		Json Execute(Json request)
		{
			const auto result = m_Commands.Execute(request);
			if (result.value("ok", false))
			{
				const auto command = request.at("command").get<std::string>();
				if (command == "project.open" || command == "project.create" || command == "project.configure")
				{
					SynchronizeProjectViews(command != "project.configure" ||
											result.at("result").at("documentChanged").get<bool>());
					m_ShowProjects = m_Commands.GetStartupError().has_value();
					if (m_ShowProjects)
					{
						RefreshProjectFields();
					}
				}
			}
			if (!result.value("ok", false))
			{
				m_Status = result.value("error", "The action failed");
			}
			else if (result.contains("result") && result.at("result").is_object() &&
					 result.at("result").contains("errors") && !result.at("result").at("errors").empty())
			{
				m_Status = result.at("result").at("errors").back().get<std::string>();
			}
			else if (result.contains("result") && result.at("result").is_object() &&
					 result.at("result").contains("warning") && !result.at("result").at("warning").is_null())
			{
				m_Status = result.at("result").at("warning").get<std::string>();
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
				RequestDocumentChange({{"command", "scene.new"}, {"name", "Untitled"}});
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
			ImGui::BeginDisabled(m_Commands.GetStartupError().has_value());
			if (ImGui::Button(m_Commands.IsPlaying() ? "Stop" : "Play"))
			{
				Execute({{"command", m_Commands.IsPlaying() ? "simulation.stop" : "simulation.start"}});
			}
			ImGui::EndDisabled();
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
				RequestDocumentChange({{"command", "scene.load"}, {"path", m_ScenePath.data()}});
			}
			ImGui::SameLine();
			if (ImGui::Button("Save"))
			{
				Save();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Recover"))
			{
				m_ScanRecovery = true;
				m_NextRecoveryScan = {};
				m_ShowRecovery = true;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(m_Commands.IsPlaying());
			if (ImGui::Button("Projects"))
			{
				FinishEdit(true);
				try
				{
					RefreshProjectFields();
					m_ShowProjects = true;
				}
				catch (const std::exception& error)
				{
					m_Status = error.what();
				}
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("Right mouse + WASD: fly   Q/E: descend/ascend   Shift: faster   Mouse wheel: speed");
			ImGui::SameLine();
			ImGui::Text(" | %s%s", m_Commands.GetScene().GetName().c_str(), m_Commands.HasUnsavedChanges() ? " *" : "");
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
			FinishEdit(true);
			Execute({{"command", "scene.save"}, {"path", m_ScenePath.data()}});
		}

		void ChangeDocument(Json request)
		{
			if (Execute(std::move(request)).value("ok", false))
			{
				SetText(m_ScenePath, m_Commands.GetScenePath().value_or("NewScene.aster"));
				m_Selected = 0;
			}
		}

		void RequestDocumentChange(Json request)
		{
			FinishEdit(true);
			if (m_Commands.HasUnsavedChanges())
			{
				m_PendingDocumentChange = std::move(request);
			}
			else
			{
				ChangeDocument(std::move(request));
			}
		}

		void RequestClose()
		{
			FinishEdit(true);
			if (m_Commands.IsPlaying())
			{
				const auto stopped = Execute({{"command", "simulation.stop"}});
				if (!stopped.value("ok", false) || !stopped.at("result").at("errors").empty())
				{
					return;
				}
			}
			RequestDocumentChange({{"command", "session.close"}});
		}

		void DrawDocumentChange()
		{
			if (m_PendingDocumentChange)
			{
				ImGui::OpenPopup("Unsaved changes");
			}
			const auto display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({480, 150});
			if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				const bool closing =
					m_PendingDocumentChange && m_PendingDocumentChange->at("command") == "session.close";
				ImGui::TextUnformatted(closing ? "Save your changes before closing Aster?"
											   : "Save your changes before changing documents?");
				bool proceed = false;
				ImGui::SetCursorPos({12, 70});
				ImGui::BeginDisabled(!m_Commands.GetScenePath());
				if (ImGui::Button("Save and continue", {150, 24}))
				{
					proceed =
						Execute({{"command", "scene.save"}, {"path", *m_Commands.GetScenePath()}}).value("ok", false);
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Discard", {135, 24}))
				{
					proceed = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", {135, 24}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					m_PendingDocumentChange.reset();
					ImGui::CloseCurrentPopup();
				}
				if (!m_Commands.GetScenePath())
				{
					ImGui::TextUnformatted("Cancel and save the new scene to choose its filename.");
				}
				if (proceed && m_PendingDocumentChange)
				{
					auto request = std::move(*m_PendingDocumentChange);
					m_PendingDocumentChange.reset();
					request["discardChanges"] = true;
					ChangeDocument(std::move(request));
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}

		void SynchronizeProjectViews(bool documentChanged)
		{
			if (m_ProjectRoot != m_Commands.GetAssetRoot())
			{
				m_ProjectRoot = m_Commands.GetAssetRoot();
				m_AssetDirectory.clear();
				m_RecoveryEntries = Json::array();
				m_SelectedRecovery.clear();
				m_ShowRecovery = false;
				m_ScanRecovery = true;
				m_NextRecoveryScan = {};
				m_RecoveryScanError.reset();
			}
			if (documentChanged)
			{
				SetText(m_ScenePath, m_Commands.GetScenePath().value_or("NewScene.aster"));
				m_Selected = 0;
				m_CachedSelected = 0;
				m_TextBuffers.clear();
			}
		}

		void RefreshProjectFields()
		{
			const auto response = m_Commands.Execute({{"command", "project.get"}});
			if (!response.value("ok", false))
			{
				throw std::runtime_error(response.at("error").get<std::string>());
			}
			const auto& project = response.at("result");
			m_HasProject = !project.at("config").is_null();
			SetText(m_ProjectLocation, m_HasProject ? project.at("path").get<std::string>() : "");
			if (m_HasProject)
			{
				const auto& config = project.at("config");
				SetText(m_ProjectName, config.at("Name").get<std::string>());
				SetText(m_ProjectAssets, config.at("AssetDirectory").get<std::string>());
				SetText(m_ProjectStartup, config.at("StartScene").get<std::string>());
			}
		}

		void DrawProjects()
		{
			if (!m_ShowProjects)
			{
				return;
			}
			const auto display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({640, 420});
			if (ImGui::Begin("Projects", &m_ShowProjects, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::BeginDisabled(m_Commands.IsPlaying());
				ImGui::SetCursorPos({12, 36});
				ImGui::TextUnformatted("Open a project file");
				ImGui::SetCursorPos({330, 32});
				if (ImGui::Button("Browse..."))
				{
					StartProjectBrowser(false);
				}
				ImGui::SetCursorPos({430, 32});
				if (ImGui::Button("Open for repair"))
				{
					RequestDocumentChange(
						{{"command", "project.open"}, {"path", m_ProjectLocation.data()}, {"repairStartup", true}});
				}
				ImGui::SetCursorPos({12, 56});
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("##project-location", m_ProjectLocation.data(), m_ProjectLocation.size());
				ImGui::SameLine();
				if (ImGui::Button("Open", {100, 0}))
				{
					RequestDocumentChange({{"command", "project.open"}, {"path", m_ProjectLocation.data()}});
				}
				ImGui::Separator();
				ImGui::SetCursorPos({12, 96});
				ImGui::TextUnformatted("Create in a new directory (its parent must exist)");
				ImGui::SetCursorPos({490, 88});
				if (ImGui::Button("Choose parent"))
				{
					StartProjectBrowser(true);
				}
				ImGui::SetCursorPos({12, 116});
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("Directory", m_NewProjectDirectory.data(), m_NewProjectDirectory.size());
				ImGui::SetCursorPos({12, 144});
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("##new-project-name", m_NewProjectName.data(), m_NewProjectName.size());
				ImGui::SameLine();
				if (ImGui::Button("Create", {100, 0}))
				{
					RequestDocumentChange({{"command", "project.create"},
										   {"path", m_NewProjectDirectory.data()},
										   {"name", m_NewProjectName.data()}});
				}
				ImGui::Separator();
				ImGui::SetCursorPos({12, 190});
				ImGui::TextUnformatted(m_Commands.GetStartupError()
										   ? "Startup repair: select a valid startup scene below"
										   : "Current project configuration");
				ImGui::SetCursorPos({12, 212});
				ImGui::BeginDisabled(!m_HasProject);
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("Name", m_ProjectName.data(), m_ProjectName.size());
				ImGui::SetCursorPos({12, 240});
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("Assets", m_ProjectAssets.data(), m_ProjectAssets.size());
				ImGui::SetCursorPos({12, 268});
				ImGui::SetNextItemWidth(490);
				ImGui::InputText("Startup scene", m_ProjectStartup.data(), m_ProjectStartup.size());
				ImGui::SetCursorPos({12, 304});
				if (ImGui::Button("Save configuration", {180, 24}))
				{
					FinishEdit(true);
					Json request = {{"command", "project.configure"},
									{"config",
									 {{"Version", 1},
									  {"Name", m_ProjectName.data()},
									  {"AssetDirectory", m_ProjectAssets.data()},
									  {"StartScene", m_ProjectStartup.data()}}}};
					const auto result = Execute(request);
					if (!result.value("ok", false) && result.value("code", "") == "unsaved_changes")
					{
						m_PendingDocumentChange = std::move(request);
					}
				}
				ImGui::EndDisabled();
				ImGui::SetCursorPos({12, 344});
				ImGui::TextWrapped(
					m_Commands.GetStartupError()
						? "Choose a valid startup scene and save configuration, or use Recover to preserve "
						  "checkpointed work."
						: "Name/startup changes keep the current scene. Changing assets opens the new startup scene.");
				if (!m_HasProject)
				{
					ImGui::TextDisabled("Open or create a project to configure it.");
				}
				ImGui::SetCursorPos({12, 388});
				if (ImGui::Button("Recent projects"))
				{
					ShowProjectLauncher();
				}
				ImGui::EndDisabled();
			}
			ImGui::End();
		}

		void RefreshProjectBrowser(const std::string& directory)
		{
			const auto response = m_Commands.Execute({{"command", "project.browse"}, {"path", directory}});
			if (!response.value("ok", false))
			{
				m_BrowserError = "Could not change directory: " + response.at("error").get<std::string>();
				return;
			}
			m_ProjectBrowser = response.at("result");
			SetText(m_BrowserDirectory, m_ProjectBrowser.at("directory").get<std::string>());
			m_BrowserSelection.clear();
			m_BrowserError.clear();
		}

		void StartProjectBrowser(bool chooseParent)
		{
			m_BrowserChooseParent = chooseParent;
			m_ProjectBrowser = Json::object();
			m_BrowserSelection.clear();
			m_BrowserError.clear();
			RefreshProjectBrowser(m_ProjectRoot.parent_path().generic_string());
			m_ShowProjectBrowser = true;
		}

		void DrawProjectBrowser()
		{
			if (!m_ShowProjectBrowser)
			{
				return;
			}
			const auto display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({680, 460});
			if (ImGui::Begin("Choose project", &m_ShowProjectBrowser,
							 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::BeginDisabled(m_Commands.IsPlaying());
				ImGui::SetCursorPos({12, 32});
				ImGui::SetNextItemWidth(500);
				const bool enter = ImGui::InputText("##browser-directory", m_BrowserDirectory.data(),
													m_BrowserDirectory.size(), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine();
				if (ImGui::Button("Go", {60, 0}) || enter)
				{
					RefreshProjectBrowser(m_BrowserDirectory.data());
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(m_ProjectBrowser.empty() || m_ProjectBrowser.at("parent").is_null());
				if (ImGui::Button("Up", {60, 0}))
				{
					RefreshProjectBrowser(m_ProjectBrowser.at("parent").get<std::string>());
				}
				ImGui::EndDisabled();
				ImGui::SetCursorPos({12, 64});
				ImGui::TextUnformatted(m_BrowserChooseParent ? "Choose the parent for a new project directory."
															 : "Select an .asterproj file. Click folders to navigate.");
				ImGui::SetCursorPos({12, 90});
				std::optional<std::string> nextDirectory;
				if (ImGui::BeginChild("Project entries", {656, 270}, true))
				{
					if (!m_ProjectBrowser.empty())
					{
						int index = 0;
						for (const auto& entry : m_ProjectBrowser.at("entries"))
						{
							const auto path = entry.at("path").get<std::string>();
							const bool directory = entry.at("directory").get<bool>();
							ImGui::PushID(index++);
							ImGui::BeginDisabled(!directory && m_BrowserChooseParent);
							const auto labelPosition = ImGui::GetCursorScreenPos();
							if (ImGui::Selectable("##entry", path == m_BrowserSelection, 0, {0, 24}))
							{
								if (directory)
								{
									nextDirectory = path;
								}
								else
								{
									m_BrowserSelection = path;
								}
							}
							const auto label =
								(directory ? "Folder: " : "Project: ") + entry.at("name").get<std::string>();
							ImGui::GetWindowDrawList()->AddText(labelPosition, ImGui::GetColorU32(ImGuiCol_Text),
																label.c_str());
							ImGui::EndDisabled();
							ImGui::PopID();
						}
					}
				}
				ImGui::EndChild();
				if (nextDirectory)
				{
					RefreshProjectBrowser(*nextDirectory);
				}
				ImGui::SetCursorPos({12, 372});
				ImGui::BeginDisabled(m_ProjectBrowser.empty() ||
									 (!m_BrowserChooseParent && m_BrowserSelection.empty()));
				if (ImGui::Button(m_BrowserChooseParent ? "Use parent directory" : "Open selected project", {190, 26}))
				{
					if (m_BrowserChooseParent)
					{
						const auto destination =
							(std::filesystem::path(m_ProjectBrowser.at("directory").get<std::string>()) / "NewGame")
								.generic_string();
						if (destination.size() >= m_NewProjectDirectory.size())
						{
							m_BrowserError = "New project directory exceeds the 4095-byte input limit.";
						}
						else
						{
							SetText(m_NewProjectDirectory, destination);
							m_ShowProjectBrowser = false;
						}
					}
					else
					{
						RequestDocumentChange(
							{{"command", "project.open"}, {"path", m_BrowserSelection}, {"repairStartup", true}});
						m_ShowProjectBrowser = false;
					}
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel", {100, 26}))
				{
					m_ShowProjectBrowser = false;
				}
				if (!m_ProjectBrowser.empty() && m_ProjectBrowser.at("truncated").get<bool>())
				{
					ImGui::TextWrapped(
						"Directory scan limited to 4096 entries. Enter a more specific directory above.");
				}
				ImGui::TextWrapped("%s", m_BrowserError.c_str());
				ImGui::EndDisabled();
			}
			ImGui::End();
		}

		void DrawProjectLauncher()
		{
			if (!m_ShowLauncher)
			{
				return;
			}
			const auto display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({680, 420});
			if (ImGui::Begin("Aster projects", &m_ShowLauncher, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::BeginDisabled(m_Commands.IsPlaying());
				ImGui::SetCursorPos({12, 32});
				if (ImGui::Button("Browse projects", {180, 26}))
				{
					m_ShowLauncher = false;
					StartProjectBrowser(false);
				}
				ImGui::SameLine();
				if (ImGui::Button("Create project", {180, 26}))
				{
					RefreshProjectFields();
					m_ShowProjects = true;
					m_ShowLauncher = false;
				}
				ImGui::SetCursorPos({12, 64});
				ImGui::TextUnformatted("Recent projects (select a path to open)");
				std::optional<std::string> forgotten;
				std::optional<std::string> opened;
				ImGui::SetCursorPos({12, 90});
				if (ImGui::BeginChild("Recent entries", {656, 270}, true))
				{
					int index = 0;
					for (const auto& entry : m_RecentProjects)
					{
						const auto path = entry.get<std::string>();
						ImGui::PushID(index++);
						if (ImGui::SmallButton("Forget"))
						{
							forgotten = path;
						}
						ImGui::SameLine();
						const auto labelPosition = ImGui::GetCursorScreenPos();
						if (ImGui::Selectable("##recent", false, 0, {0, 24}))
						{
							opened = path;
						}
						ImGui::GetWindowDrawList()->AddText(labelPosition, ImGui::GetColorU32(ImGuiCol_Text),
															path.c_str());
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("%s", path.c_str());
						}
						ImGui::PopID();
					}
					if (m_RecentProjects.empty())
					{
						ImGui::TextUnformatted("No recent projects yet. Browse or create a project.");
					}
				}
				ImGui::EndChild();
				if (forgotten)
				{
					const auto result = Execute({{"command", "project.forget"}, {"path", *forgotten}});
					if (result.value("ok", false))
					{
						ShowProjectLauncher();
					}
				}
				if (opened)
				{
					m_ShowLauncher = false;
					RequestDocumentChange({{"command", "project.open"}, {"path", *opened}, {"repairStartup", true}});
				}
				ImGui::TextWrapped("%s", m_LauncherError.c_str());
				ImGui::EndDisabled();
			}
			ImGui::End();
		}

		void DrawRecovery()
		{
			const auto now = std::chrono::steady_clock::now();
			if (m_ScanRecovery && now >= m_NextRecoveryScan)
			{
				m_ScanRecovery = false;
				const auto response = m_Commands.Execute({{"command", "recovery.list"}});
				if (response.value("ok", false))
				{
					if (m_RecoveryScanError && m_Status == *m_RecoveryScanError)
					{
						m_Status = "Ready";
					}
					m_RecoveryScanError.reset();
					m_RecoveryEntries = response.at("result");
					for (const auto& entry : m_RecoveryEntries)
					{
						if (!entry.at("active").get<bool>())
						{
							m_ShowRecovery = true;
						}
					}
				}
				else
				{
					m_Status = response.at("error").get<std::string>();
					m_RecoveryScanError = m_Status;
					m_ScanRecovery = true;
					m_NextRecoveryScan = now + std::chrono::seconds(5);
				}
			}
			if (m_ShowRecovery)
			{
				ImGui::OpenPopup("Recover scenes");
			}
			const auto display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({560, 320});
			if (ImGui::BeginPopupModal("Recover scenes", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::TextUnformatted("Unsaved scenes from previous editor sessions");
				ImGui::TextDisabled("Recovery keeps the checkpoint until you explicitly discard it.");
				ImGui::BeginChild("Recovered scenes", {0, 180}, true);
				bool recoverable = false;
				bool discardable = false;
				for (const auto& entry : m_RecoveryEntries)
				{
					const auto id = entry.at("id").get<std::string>();
					const bool active = entry.at("active").get<bool>();
					ImGui::PushID(id.c_str());
					ImGui::BeginDisabled(active);
					const auto name = entry.at("name").is_string() ? entry.at("name").get<std::string>()
									  : active					   ? "Active editor session"
																   : "Unreadable checkpoint";
					if (ImGui::Selectable(name.c_str(), m_SelectedRecovery == id))
					{
						m_SelectedRecovery = id;
					}
					ImGui::EndDisabled();
					if (entry.at("path").is_string())
					{
						ImGui::TextDisabled("%s", entry.at("path").get<std::string>().c_str());
					}
					if (entry.at("error").is_string())
					{
						ImGui::TextWrapped("%s", entry.at("error").get<std::string>().c_str());
					}
					if (m_SelectedRecovery == id && !active)
					{
						discardable = true;
						recoverable = entry.at("error").is_null();
					}
					ImGui::PopID();
				}
				if (m_RecoveryEntries.empty())
				{
					ImGui::TextUnformatted("No recovery checkpoints are available.");
				}
				ImGui::EndChild();
				ImGui::BeginDisabled(!recoverable || m_Commands.IsPlaying());
				if (ImGui::Button("Recover copy", {150, 24}))
				{
					m_ShowRecovery = false;
					ImGui::CloseCurrentPopup();
					RequestDocumentChange({{"command", "recovery.restore"}, {"session", m_SelectedRecovery}});
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!discardable);
				if (ImGui::Button("Discard checkpoint", {180, 24}))
				{
					if (Execute({{"command", "recovery.discard"}, {"session", m_SelectedRecovery}}).value("ok", false))
					{
						m_SelectedRecovery.clear();
						m_ScanRecovery = true;
					}
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Later", {150, 24}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					m_ShowRecovery = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
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
				const float environmentFieldWidth =
					std::max(40.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Sky rotation (radians)").x -
										ImGui::GetStyle().ItemInnerSpacing.x);
				ImGui::SetNextItemWidth(environmentFieldWidth);
				if (ImGui::DragFloat("Sky intensity", &environment.Intensity, 0.01f, 0.0f, 100.0f))
				{
					if (ImGui::IsItemActive())
					{
						BeginEdit(EditKind::Inspector);
					}
					Execute(
						{{"command", "scene.environment"}, {"environment", {{"Intensity", environment.Intensity}}}});
				}
				ImGui::SetNextItemWidth(environmentFieldWidth);
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
				RequestDocumentChange({{"command", "scene.load"}, {"path", relative.generic_string()}});
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
					if (!IsEditorMetadataName(entry.path().filename().string()))
					{
						entries.push_back(entry);
					}
				}
				std::sort(entries.begin(), entries.end(),
						  [](const auto& first, const auto& second) { return first.path() < second.path(); });
				std::optional<std::filesystem::path> openDirectory;
				for (const auto& entry : entries)
				{
					const auto canonical = std::filesystem::canonical(entry.path());
					if (!IsWithin(m_ProjectRoot, canonical) ||
						ContainsEditorMetadata(canonical.lexically_relative(m_ProjectRoot)))
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
							 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing |
							 ImGuiWindowFlags_NoNavFocus);
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
		std::array<char, 4096> m_ProjectLocation{};
		std::array<char, 4096> m_NewProjectDirectory{};
		std::array<char, 129> m_NewProjectName{};
		std::array<char, 129> m_ProjectName{};
		std::array<char, 4096> m_ProjectAssets{};
		std::array<char, 4096> m_ProjectStartup{};
		bool m_ShowProjects = false;
		bool m_ShowProjectBrowser = false;
		bool m_BrowserChooseParent = false;
		bool m_ShowLauncher = false;
		std::array<char, 4096> m_BrowserDirectory{};
		std::string m_BrowserSelection;
		std::string m_BrowserError;
		std::string m_LauncherError;
		Json m_ProjectBrowser = Json::object();
		Json m_RecentProjects = Json::array();
		bool m_HasProject = false;
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
		std::optional<Json> m_PendingDocumentChange;
		Json m_RecoveryEntries = Json::array();
		std::string m_SelectedRecovery;
		bool m_ScanRecovery = true;
		std::chrono::steady_clock::time_point m_NextRecoveryScan{};
		std::optional<std::string> m_RecoveryScanError;
		bool m_ShowRecovery = false;
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
	void GuiLayer::ShowProjectLauncher()
	{
		m_Impl->ShowProjectLauncher();
	}
} // namespace Aster
