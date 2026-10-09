#include "SceneRenderPass.h"
#include <Aster/Renderer/Renderer.h>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Aster
{
	namespace
	{
		std::mutex s_GlfwMutex;
		uint32_t s_GlfwUsers = 0;
		std::thread::id s_GlfwThread;
		std::mutex s_RendererMutex;
		bool s_RendererActive = false;

		void CheckVulkan(VkResult result, const char* operation)
		{
			if (result != VK_SUCCESS)
			{
				throw std::runtime_error(std::string(operation) + " failed: " + nvrhi::vulkan::resultToString(result));
			}
		}

		void ValidateDimensions(uint32_t width, uint32_t height)
		{
			if (width == 0 || height == 0 || width > 16384 || height > 16384)
			{
				throw std::invalid_argument("Renderer dimensions must be between 1 and 16384 pixels");
			}
		}

		std::string GlfwError()
		{
			const char* description = nullptr;
			glfwGetError(&description);
			return description ? description : "GLFW did not provide an error description";
		}

		void AcquireGlfw()
		{
			std::lock_guard lock(s_GlfwMutex);
			if (s_GlfwUsers != 0 && s_GlfwThread != std::this_thread::get_id())
			{
				throw std::logic_error("All GLFW windows must belong to the same application thread");
			}
			if (s_GlfwUsers == 0)
			{
				if (glfwInit() != GLFW_TRUE)
				{
					throw std::runtime_error("GLFW initialization failed: " + GlfwError());
				}
				s_GlfwThread = std::this_thread::get_id();
			}
			++s_GlfwUsers;
		}

		void ReleaseGlfw()
		{
			std::lock_guard lock(s_GlfwMutex);
			if (--s_GlfwUsers == 0)
			{
				glfwTerminate();
				s_GlfwThread = {};
			}
		}

		bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
		{
			return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension)
							   { return std::strcmp(extension.extensionName, name) == 0; });
		}

		std::string KeyName(int key)
		{
			if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
			{
				return std::string(1, static_cast<char>('A' + key - GLFW_KEY_A));
			}
			if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
			{
				return std::string(1, static_cast<char>('0' + key - GLFW_KEY_0));
			}
			if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25)
			{
				return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
			}
			if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
			{
				return "Keypad" + std::to_string(key - GLFW_KEY_KP_0);
			}
			switch (key)
			{
			case GLFW_KEY_SPACE:
				return "Space";
			case GLFW_KEY_APOSTROPHE:
				return "Apostrophe";
			case GLFW_KEY_COMMA:
				return "Comma";
			case GLFW_KEY_MINUS:
				return "Minus";
			case GLFW_KEY_PERIOD:
				return "Period";
			case GLFW_KEY_SLASH:
				return "Slash";
			case GLFW_KEY_SEMICOLON:
				return "Semicolon";
			case GLFW_KEY_EQUAL:
				return "Equal";
			case GLFW_KEY_LEFT_BRACKET:
				return "LeftBracket";
			case GLFW_KEY_BACKSLASH:
				return "Backslash";
			case GLFW_KEY_RIGHT_BRACKET:
				return "RightBracket";
			case GLFW_KEY_GRAVE_ACCENT:
				return "GraveAccent";
			case GLFW_KEY_WORLD_1:
				return "World1";
			case GLFW_KEY_WORLD_2:
				return "World2";
			case GLFW_KEY_ESCAPE:
				return "Escape";
			case GLFW_KEY_ENTER:
				return "Enter";
			case GLFW_KEY_TAB:
				return "Tab";
			case GLFW_KEY_BACKSPACE:
				return "Backspace";
			case GLFW_KEY_INSERT:
				return "Insert";
			case GLFW_KEY_DELETE:
				return "Delete";
			case GLFW_KEY_RIGHT:
				return "Right";
			case GLFW_KEY_LEFT:
				return "Left";
			case GLFW_KEY_DOWN:
				return "Down";
			case GLFW_KEY_UP:
				return "Up";
			case GLFW_KEY_PAGE_UP:
				return "PageUp";
			case GLFW_KEY_PAGE_DOWN:
				return "PageDown";
			case GLFW_KEY_HOME:
				return "Home";
			case GLFW_KEY_END:
				return "End";
			case GLFW_KEY_CAPS_LOCK:
				return "CapsLock";
			case GLFW_KEY_SCROLL_LOCK:
				return "ScrollLock";
			case GLFW_KEY_NUM_LOCK:
				return "NumLock";
			case GLFW_KEY_PRINT_SCREEN:
				return "PrintScreen";
			case GLFW_KEY_PAUSE:
				return "Pause";
			case GLFW_KEY_KP_DECIMAL:
				return "KeypadDecimal";
			case GLFW_KEY_KP_DIVIDE:
				return "KeypadDivide";
			case GLFW_KEY_KP_MULTIPLY:
				return "KeypadMultiply";
			case GLFW_KEY_KP_SUBTRACT:
				return "KeypadSubtract";
			case GLFW_KEY_KP_ADD:
				return "KeypadAdd";
			case GLFW_KEY_KP_ENTER:
				return "KeypadEnter";
			case GLFW_KEY_KP_EQUAL:
				return "KeypadEqual";
			case GLFW_KEY_LEFT_SHIFT:
				return "LeftShift";
			case GLFW_KEY_LEFT_CONTROL:
				return "LeftControl";
			case GLFW_KEY_LEFT_ALT:
				return "LeftAlt";
			case GLFW_KEY_LEFT_SUPER:
				return "LeftSuper";
			case GLFW_KEY_RIGHT_SHIFT:
				return "RightShift";
			case GLFW_KEY_RIGHT_CONTROL:
				return "RightControl";
			case GLFW_KEY_RIGHT_ALT:
				return "RightAlt";
			case GLFW_KEY_RIGHT_SUPER:
				return "RightSuper";
			case GLFW_KEY_MENU:
				return "Menu";
			default:
				return {};
			}
		}
	} // namespace

	class Renderer::Impl final : public nvrhi::IMessageCallback
	{
	  public:
		explicit Impl(const RendererOptions& options) : m_Options(options), m_Thread(std::this_thread::get_id()) {}

		~Impl() override
		{
			Shutdown();
		}

		void Initialize()
		{
			ValidateDimensions(m_Options.Width, m_Options.Height);
			if (m_Options.Title.find('\0') != std::string::npos || m_Options.DeviceName.find('\0') != std::string::npos)
			{
				throw std::invalid_argument("Window title and device name must not contain NUL characters");
			}
			{
				std::lock_guard lock(s_RendererMutex);
				if (s_RendererActive)
				{
					throw std::logic_error("Aster supports one active Renderer device; share it between viewports");
				}
				s_RendererActive = true;
				m_HasDeviceLease = true;
			}
			if (!m_Options.Headless)
			{
				AcquireGlfw();
				m_HasGlfw = true;
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
				glfwWindowHint(GLFW_VISIBLE, m_Options.Visible ? GLFW_TRUE : GLFW_FALSE);
				m_Window = glfwCreateWindow(static_cast<int>(m_Options.Width), static_cast<int>(m_Options.Height),
											m_Options.Title.c_str(), nullptr, nullptr);
				if (!m_Window)
				{
					throw std::runtime_error("GLFW window creation failed: " + GlfwError());
				}
				InstallInputCallbacks();
			}
			CreateInstance();
			if (m_Window)
			{
				CheckVulkan(glfwCreateWindowSurface(m_Instance, m_Window, nullptr, &m_Surface),
							"Creating window surface");
			}
			CreateDevice();
			CreateNvrhi();
			if (m_Surface)
			{
				CreateSwapchain();
			}
			CreateRenderTarget(m_Options.Width, m_Options.Height);
			CheckBackendErrors();
		}

		void Shutdown() noexcept
		{
			if (m_Device)
			{
				const auto result = vkDeviceWaitIdle(m_Device);
				if (result != VK_SUCCESS)
				{
					Record("Vulkan device wait failed during shutdown", true);
				}
				m_CommandList = nullptr;
				m_OverlayCallback = {};
				m_OverlayFramebuffer = nullptr;
				m_ScenePass.reset();
				m_RenderTarget = nullptr;
				m_StagingTexture = nullptr;
				DestroySwapchain();
				m_NvrhiDevice = nullptr;
				m_VulkanDevice = nullptr;
				vkDestroyDevice(m_Device, nullptr);
				m_Device = VK_NULL_HANDLE;
			}
			if (m_Surface)
			{
				vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
				m_Surface = VK_NULL_HANDLE;
			}
			if (m_DebugMessenger)
			{
				const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
					vkGetInstanceProcAddr(m_Instance, "vkDestroyDebugUtilsMessengerEXT"));
				destroyMessenger(m_Instance, m_DebugMessenger, nullptr);
				m_DebugMessenger = VK_NULL_HANDLE;
			}
			if (m_Instance)
			{
				vkDestroyInstance(m_Instance, nullptr);
				m_Instance = VK_NULL_HANDLE;
			}
			if (m_Window)
			{
				glfwDestroyWindow(m_Window);
				m_Window = nullptr;
			}
			if (m_HasGlfw)
			{
				ReleaseGlfw();
				m_HasGlfw = false;
			}
			if (m_HasDeviceLease)
			{
				std::lock_guard lock(s_RendererMutex);
				s_RendererActive = false;
				m_HasDeviceLease = false;
			}
		}

		void RenderFrame(const std::array<float, 4>& clearColor, const Scene* scene = nullptr,
						 const AssetImporter* importer = nullptr, const RenderSettings& settings = {})
		{
			CheckActive();
			CheckOutsideFrame();
			struct FrameGuard
			{
				bool& Active;
				explicit FrameGuard(bool& active) : Active(active)
				{
					Active = true;
				}
				~FrameGuard()
				{
					Active = false;
				}
			} frameGuard(m_InFrame);
			for (const auto channel : clearColor)
			{
				if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f)
				{
					throw std::invalid_argument("Clear color channels must be finite and between zero and one");
				}
			}
			uint32_t imageIndex = 0;
			if (m_Window)
			{
				int width = 0;
				int height = 0;
				glfwGetFramebufferSize(m_Window, &width, &height);
				if (width == 0 || height == 0)
				{
					return;
				}
				if (static_cast<uint32_t>(width) != m_Options.Width ||
					static_cast<uint32_t>(height) != m_Options.Height)
				{
					RecreateSwapchain();
				}
			}
			const auto prepareScene = [&]()
			{
				if (scene)
				{
					try
					{
						if (!m_ScenePass)
						{
							m_ScenePass = std::make_unique<SceneRenderPass>(m_NvrhiDevice);
						}
						m_ScenePass->Prepare(*scene, *importer, m_RenderTarget, settings);
					}
					catch (...)
					{
						CheckBackendErrors();
						throw;
					}
					CheckBackendErrors();
				}
			};
			prepareScene();
			if (m_Window)
			{
				auto result = vkAcquireNextImageKHR(m_Device, m_Swapchain, std::numeric_limits<uint64_t>::max(),
													m_ImageAvailable, VK_NULL_HANDLE, &imageIndex);
				if (result == VK_ERROR_OUT_OF_DATE_KHR)
				{
					RecreateSwapchain();
					prepareScene();
					result = vkAcquireNextImageKHR(m_Device, m_Swapchain, std::numeric_limits<uint64_t>::max(),
												   m_ImageAvailable, VK_NULL_HANDLE, &imageIndex);
				}
				if (result != VK_SUBOPTIMAL_KHR)
				{
					CheckVulkan(result, "Acquiring swapchain image");
				}
				m_VulkanDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, m_ImageAvailable, 0);
				m_VulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, m_RenderFinished[imageIndex], 0);
			}
			m_CommandList->open();
			if (scene)
			{
				m_ScenePass->Record(m_CommandList);
			}
			else
			{
				m_CommandList->clearTextureFloat(
					m_RenderTarget, nvrhi::AllSubresources,
					nvrhi::Color(clearColor[0], clearColor[1], clearColor[2], clearColor[3]));
			}
			std::exception_ptr overlayError;
			const auto overlayCallback = m_OverlayCallback;
			if (overlayCallback)
			{
				try
				{
					overlayCallback(m_NvrhiDevice, m_CommandList, m_OverlayFramebuffer);
				}
				catch (...)
				{
					// Finish acquired-image synchronization before propagating user callback failures.
					overlayError = std::current_exception();
				}
			}
			if (m_Window)
			{
				// Acquired images may be discarded; an undefined initial state also covers first use.
				m_CommandList->beginTrackingTextureState(m_SwapchainTextures[imageIndex], nvrhi::AllSubresources,
														 nvrhi::ResourceStates::Common);
				m_CommandList->copyTexture(m_SwapchainTextures[imageIndex], {}, m_RenderTarget, {});
				m_CommandList->setTextureState(m_SwapchainTextures[imageIndex], nvrhi::AllSubresources,
											   nvrhi::ResourceStates::Present);
				m_CommandList->commitBarriers();
			}
			m_CommandList->close();
			m_NvrhiDevice->executeCommandList(m_CommandList);
			if (m_Window)
			{
				VkPresentInfoKHR present{};
				present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
				present.waitSemaphoreCount = 1;
				present.pWaitSemaphores = &m_RenderFinished[imageIndex];
				present.swapchainCount = 1;
				present.pSwapchains = &m_Swapchain;
				present.pImageIndices = &imageIndex;
				const auto result = vkQueuePresentKHR(m_Queue, &present);
				if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)
				{
					CheckVulkan(result, "Presenting swapchain image");
				}
			}
			WaitForIdle();
			m_HasRendered = true;
			m_NvrhiDevice->runGarbageCollection();
			CheckBackendErrors();
			if (overlayError)
			{
				std::rethrow_exception(overlayError);
			}
		}

		RenderImage ReadbackRgba8()
		{
			CheckActive();
			CheckOutsideFrame();
			if (!m_HasRendered)
			{
				throw std::logic_error("Render a frame before requesting pixel readback");
			}
			m_CommandList->open();
			m_CommandList->copyTexture(m_StagingTexture, {}, m_RenderTarget, {});
			m_CommandList->close();
			m_NvrhiDevice->executeCommandList(m_CommandList);
			WaitForIdle();
			RenderImage image;
			image.Width = m_Options.Width;
			image.Height = m_Options.Height;
			const size_t rowBytes = static_cast<size_t>(image.Width) * 4;
			image.Pixels.resize(rowBytes * image.Height);
			size_t rowPitch = 0;
			const auto* pixels = static_cast<const uint8_t*>(
				m_NvrhiDevice->mapStagingTexture(m_StagingTexture, {}, nvrhi::CpuAccessMode::Read, &rowPitch));
			if (!pixels)
			{
				throw std::runtime_error("Mapping Vulkan staging texture failed");
			}
			for (uint32_t row = 0; row < image.Height; ++row)
			{
				std::memcpy(image.Pixels.data() + rowBytes * row, pixels + rowPitch * row, rowBytes);
			}
			m_NvrhiDevice->unmapStagingTexture(m_StagingTexture);
			if (m_Format == nvrhi::Format::BGRA8_UNORM)
			{
				for (size_t index = 0; index < image.Pixels.size(); index += 4)
				{
					std::swap(image.Pixels[index], image.Pixels[index + 2]);
				}
			}
			m_NvrhiDevice->runGarbageCollection();
			CheckBackendErrors();
			return image;
		}

		void Resize(uint32_t width, uint32_t height)
		{
			CheckActive();
			CheckOutsideFrame();
			ValidateDimensions(width, height);
			WaitForIdle();
			if (m_Window)
			{
				glfwSetWindowSize(m_Window, static_cast<int>(width), static_cast<int>(height));
				glfwPollEvents();
				RecreateSwapchain();
			}
			else
			{
				CreateRenderTarget(width, height);
			}
		}

		void PollEvents()
		{
			CheckActive();
			m_Input.BeginFrame();
			if (m_Window)
			{
				glfwPollEvents();
			}
			CheckBackendErrors();
		}

		InputSnapshot GetInputSnapshot() const
		{
			CheckActive();
			return m_Input.GetSnapshot();
		}

		bool ShouldClose() const
		{
			CheckActive();
			return m_Window && glfwWindowShouldClose(m_Window);
		}

		std::string GetDeviceName() const
		{
			return m_DeviceName;
		}

		uint32_t GetWidth() const
		{
			return m_Options.Width;
		}

		uint32_t GetHeight() const
		{
			return m_Options.Height;
		}

		std::vector<std::string> GetValidationMessages() const
		{
			std::lock_guard lock(m_MessageMutex);
			auto messages = m_Messages;
			if (m_LostMessage.load())
			{
				messages.emplace_back("Renderer could not store a validation diagnostic");
			}
			return messages;
		}

		void CheckThread() const
		{
			if (m_Thread != std::this_thread::get_id())
			{
				throw std::logic_error("Renderer operations must run on the constructing thread");
			}
		}

		void CheckOutsideFrame() const
		{
			if (m_InFrame)
			{
				throw std::logic_error(
					"Renderer lifetime and frame operations cannot be reentered from a render callback");
			}
		}

		void SetOverlayCallback(OverlayCallback callback)
		{
			CheckActive();
			m_OverlayCallback = std::move(callback);
		}

		void* GetNativeWindow() const
		{
			CheckActive();
			return m_Window;
		}

		void InvalidateAssets()
		{
			CheckActive();
			CheckOutsideFrame();
			WaitForIdle();
			if (m_ScenePass)
			{
				m_ScenePass->InvalidateAssets();
			}
		}

		void SetEnvironment(const EnvironmentMaps& environment)
		{
			CheckActive();
			CheckOutsideFrame();
			WaitForIdle();
			if (!m_ScenePass)
			{
				m_ScenePass = std::make_unique<SceneRenderPass>(m_NvrhiDevice);
			}
			m_ScenePass->SetEnvironment(environment);
			CheckBackendErrors();
		}

	  private:
		template <typename Callback> static void DispatchInput(GLFWwindow* window, Callback callback) noexcept
		{
			auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(window));
			try
			{
				callback(renderer->m_Input);
			}
			catch (const std::exception& error)
			{
				renderer->Record(error.what(), true);
			}
			catch (...)
			{
				renderer->Record("Unknown failure in GLFW input callback", true);
			}
		}

		void InstallInputCallbacks()
		{
			glfwSetWindowUserPointer(m_Window, this);
			m_Input.FocusEvent(glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == GLFW_TRUE);
			double x = 0;
			double y = 0;
			glfwGetCursorPos(m_Window, &x, &y);
			m_Input.CursorEvent(x, y);
			glfwSetKeyCallback(m_Window,
							   [](GLFWwindow* window, int key, int, int action, int)
							   {
								   DispatchInput(window,
												 [=](InputState& input)
												 {
													 const auto name = KeyName(key);
													 if (!name.empty())
													 {
														 input.KeyEvent(name, action != GLFW_RELEASE);
													 }
												 });
							   });
			glfwSetMouseButtonCallback(
				m_Window,
				[](GLFWwindow* window, int button, int action, int)
				{
					DispatchInput(window, [=](InputState& input)
								  { input.MouseButtonEvent(static_cast<size_t>(button), action != GLFW_RELEASE); });
				});
			glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y)
									 { DispatchInput(window, [=](InputState& input) { input.CursorEvent(x, y); }); });
			glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double x, double y)
								  { DispatchInput(window, [=](InputState& input) { input.ScrollEvent(x, y); }); });
			glfwSetWindowFocusCallback(
				m_Window, [](GLFWwindow* window, int focused)
				{ DispatchInput(window, [=](InputState& input) { input.FocusEvent(focused == GLFW_TRUE); }); });
		}

		void message(nvrhi::MessageSeverity severity, const char* text) noexcept override
		{
			if (severity >= nvrhi::MessageSeverity::Warning)
			{
				Record(text, severity >= nvrhi::MessageSeverity::Error);
			}
		}

		void Record(const char* text, bool error) noexcept
		{
			if (error)
			{
				m_HasError.store(true);
			}
			try
			{
				std::lock_guard lock(m_MessageMutex);
				m_Messages.emplace_back(text ? text : "Renderer received an empty diagnostic");
			}
			catch (...)
			{
				// Exceptions must not cross the Vulkan driver callback boundary.
				m_LostMessage.store(true);
				std::fputs("Aster: could not store Vulkan/NVRHI diagnostic\n", stderr);
			}
		}

		static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
															VkDebugUtilsMessageTypeFlagsEXT,
															const VkDebugUtilsMessengerCallbackDataEXT* data,
															void* context) noexcept
		{
			auto* renderer = static_cast<Impl*>(context);
			renderer->Record(data->pMessage, severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
			return VK_FALSE;
		}

		void CheckBackendErrors() const
		{
			if (m_HasError.load() || m_LostMessage.load())
			{
				const auto messages = GetValidationMessages();
				throw std::runtime_error("Vulkan/NVRHI reported an error: " +
										 (messages.empty() ? "diagnostic unavailable" : messages.back()));
			}
		}

		void CheckActive() const
		{
			CheckThread();
			if (!m_NvrhiDevice)
			{
				throw std::logic_error("Renderer has been shut down");
			}
			CheckBackendErrors();
		}

		void WaitForIdle()
		{
			CheckVulkan(vkDeviceWaitIdle(m_Device), "Waiting for Vulkan device");
		}

		void CreateInstance()
		{
			uint32_t version = VK_API_VERSION_1_0;
			const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
				vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
			if (enumerateVersion)
			{
				CheckVulkan(enumerateVersion(&version), "Querying Vulkan version");
			}
			if (version < VK_API_VERSION_1_3)
			{
				throw std::runtime_error("Aster's NVRHI backend requires Vulkan 1.3 or newer");
			}
			uint32_t extensionCount = 0;
			CheckVulkan(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
						"Enumerating Vulkan extensions");
			std::vector<VkExtensionProperties> availableExtensions(extensionCount);
			CheckVulkan(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data()),
						"Reading Vulkan extensions");
			if (m_Window)
			{
				uint32_t glfwCount = 0;
				const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
				if (!glfwExtensions)
				{
					throw std::runtime_error("GLFW cannot find Vulkan surface extensions: " + GlfwError());
				}
				m_InstanceExtensions.assign(glfwExtensions, glfwExtensions + glfwCount);
			}
			VkInstanceCreateFlags flags = 0;
			if (HasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
			{
				m_InstanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
				flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
			}
			std::vector<const char*> layers;
			VkDebugUtilsMessengerCreateInfoEXT debug{};
			debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			debug.messageSeverity =
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
								VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
								VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			debug.pfnUserCallback = DebugCallback;
			debug.pUserData = this;
			const VkValidationFeatureEnableEXT validationFeature =
				VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
			VkValidationFeaturesEXT validationFeatures{};
			validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
			validationFeatures.pNext = &debug;
			validationFeatures.enabledValidationFeatureCount = 1;
			validationFeatures.pEnabledValidationFeatures = &validationFeature;
			if (m_Options.EnableValidation)
			{
				uint32_t layerCount = 0;
				CheckVulkan(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "Enumerating Vulkan layers");
				std::vector<VkLayerProperties> availableLayers(layerCount);
				CheckVulkan(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()),
							"Reading Vulkan layers");
				if (!std::any_of(availableLayers.begin(), availableLayers.end(), [](const auto& layer)
								 { return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; }))
				{
					throw std::runtime_error(
						"Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
				}
				layers.push_back("VK_LAYER_KHRONOS_validation");
				uint32_t layerExtensionCount = 0;
				CheckVulkan(vkEnumerateInstanceExtensionProperties(layers.front(), &layerExtensionCount, nullptr),
							"Enumerating validation extensions");
				std::vector<VkExtensionProperties> layerExtensions(layerExtensionCount);
				CheckVulkan(vkEnumerateInstanceExtensionProperties(layers.front(), &layerExtensionCount,
																   layerExtensions.data()),
							"Reading validation extensions");
				availableExtensions.insert(availableExtensions.end(), layerExtensions.begin(), layerExtensions.end());
				m_InstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				m_InstanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
			}
			for (const auto* extension : m_InstanceExtensions)
			{
				if (!HasExtension(availableExtensions, extension))
				{
					throw std::runtime_error(std::string("Required Vulkan instance extension is unavailable: ") +
											 extension);
				}
			}
			VkApplicationInfo application{};
			application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			application.pApplicationName = "Aster";
			application.pEngineName = "Aster";
			application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
			application.apiVersion = VK_API_VERSION_1_3;
			VkInstanceCreateInfo create{};
			create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			create.pApplicationInfo = &application;
			create.flags = flags;
			create.enabledExtensionCount = static_cast<uint32_t>(m_InstanceExtensions.size());
			create.ppEnabledExtensionNames = m_InstanceExtensions.data();
			create.enabledLayerCount = static_cast<uint32_t>(layers.size());
			create.ppEnabledLayerNames = layers.data();
			create.pNext = m_Options.EnableValidation ? &validationFeatures : nullptr;
			CheckVulkan(vkCreateInstance(&create, nullptr, &m_Instance), "Creating Vulkan instance");
			if (m_Options.EnableValidation)
			{
				const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
					vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT"));
				if (!createMessenger)
				{
					throw std::runtime_error("Vulkan debug utils entry point is unavailable");
				}
				CheckVulkan(createMessenger(m_Instance, &debug, nullptr, &m_DebugMessenger),
							"Creating Vulkan validation messenger");
			}
		}

		void CreateDevice()
		{
			uint32_t deviceCount = 0;
			CheckVulkan(vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr), "Enumerating Vulkan devices");
			std::vector<VkPhysicalDevice> devices(deviceCount);
			CheckVulkan(vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data()), "Reading Vulkan devices");
			int selectedScore = -1;
			for (const auto physicalDevice : devices)
			{
				VkPhysicalDeviceProperties properties{};
				vkGetPhysicalDeviceProperties(physicalDevice, &properties);
				if (!m_Options.DeviceName.empty() && m_Options.DeviceName != properties.deviceName)
				{
					continue;
				}
				if (properties.apiVersion < VK_API_VERSION_1_3)
				{
					continue;
				}
				VkPhysicalDeviceVulkan13Features features13{};
				features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
				VkPhysicalDeviceVulkan12Features features12{};
				features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
				features12.pNext = &features13;
				VkPhysicalDeviceFeatures2 features{};
				features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
				features.pNext = &features12;
				vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
				if (!features12.timelineSemaphore || !features13.synchronization2 || !features13.dynamicRendering ||
					!features13.shaderDemoteToHelperInvocation)
				{
					continue;
				}
				uint32_t extensionCount = 0;
				CheckVulkan(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr),
							"Enumerating device extensions");
				std::vector<VkExtensionProperties> extensions(extensionCount);
				CheckVulkan(
					vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()),
					"Reading device extensions");
				if (m_Surface && !HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
				{
					continue;
				}
				uint32_t familyCount = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
				std::vector<VkQueueFamilyProperties> families(familyCount);
				vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
				for (uint32_t index = 0; index < familyCount; ++index)
				{
					VkBool32 presentSupported = VK_TRUE;
					if (m_Surface)
					{
						CheckVulkan(
							vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, index, m_Surface, &presentSupported),
							"Querying surface presentation support");
					}
					if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || !presentSupported)
					{
						continue;
					}
					const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
										  ? 2
										  : (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0);
					if (score > selectedScore)
					{
						selectedScore = score;
						m_PhysicalDevice = physicalDevice;
						m_QueueFamily = index;
						m_DeviceName = properties.deviceName;
						m_MaxImageDimension = properties.limits.maxImageDimension2D;
						m_DeviceExtensions.clear();
						if (m_Surface)
						{
							m_DeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
						}
						if (HasExtension(extensions, "VK_KHR_portability_subset"))
						{
							m_DeviceExtensions.push_back("VK_KHR_portability_subset");
						}
					}
					break;
				}
			}
			if (!m_PhysicalDevice)
			{
				throw std::runtime_error(
					"No matching Vulkan 1.3 device supports graphics, timeline semaphores, dynamic rendering, "
					"synchronization2, fragment demotion, and requested presentation; requested device: " +
					(m_Options.DeviceName.empty() ? std::string("automatic selection") : m_Options.DeviceName));
			}
			const float priority = 1.0f;
			VkDeviceQueueCreateInfo queue{};
			queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue.queueFamilyIndex = m_QueueFamily;
			queue.queueCount = 1;
			queue.pQueuePriorities = &priority;
			VkPhysicalDeviceVulkan13Features features13{};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			features13.synchronization2 = VK_TRUE;
			features13.dynamicRendering = VK_TRUE;
			// Vulkan 1.3 glslang emits fragment demotion for alpha-mask discard.
			// Core feature support is mandatory, but device use must still be enabled.
			features13.shaderDemoteToHelperInvocation = VK_TRUE;
			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			features12.pNext = &features13;
			features12.timelineSemaphore = VK_TRUE;
			VkDeviceCreateInfo create{};
			create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			create.pNext = &features12;
			create.queueCreateInfoCount = 1;
			create.pQueueCreateInfos = &queue;
			create.enabledExtensionCount = static_cast<uint32_t>(m_DeviceExtensions.size());
			create.ppEnabledExtensionNames = m_DeviceExtensions.data();
			CheckVulkan(vkCreateDevice(m_PhysicalDevice, &create, nullptr, &m_Device), "Creating Vulkan device");
			vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, vkGetInstanceProcAddr);
		}

		void CreateNvrhi()
		{
			nvrhi::vulkan::DeviceDesc description{};
			description.errorCB = this;
			description.instance = m_Instance;
			description.physicalDevice = m_PhysicalDevice;
			description.device = m_Device;
			description.graphicsQueue = m_Queue;
			description.graphicsQueueIndex = static_cast<int>(m_QueueFamily);
			description.instanceExtensions = m_InstanceExtensions.data();
			description.numInstanceExtensions = m_InstanceExtensions.size();
			description.deviceExtensions = m_DeviceExtensions.data();
			description.numDeviceExtensions = m_DeviceExtensions.size();
			m_VulkanDevice = nvrhi::vulkan::createDevice(description);
			if (!m_VulkanDevice)
			{
				throw std::runtime_error("NVRHI Vulkan device creation failed");
			}
			m_NvrhiDevice =
				m_Options.EnableValidation ? nvrhi::validation::createValidationLayer(m_VulkanDevice) : m_VulkanDevice;
			if (!m_NvrhiDevice)
			{
				throw std::runtime_error("NVRHI validation device creation failed");
			}
			m_CommandList = m_NvrhiDevice->createCommandList();
			if (!m_CommandList)
			{
				throw std::runtime_error("NVRHI command list creation failed");
			}
		}

		void CreateRenderTarget(uint32_t width, uint32_t height)
		{
			if (width > m_MaxImageDimension || height > m_MaxImageDimension)
			{
				throw std::invalid_argument("Render target exceeds the device's maximum texture dimensions");
			}
			nvrhi::TextureDesc description;
			description.width = width;
			description.height = height;
			description.format = m_Format;
			description.isRenderTarget = true;
			description.debugName = "Aster color target";
			description.enableAutomaticStateTracking(nvrhi::ResourceStates::CopySource);
			auto target = m_NvrhiDevice->createTexture(description);
			auto staging = m_NvrhiDevice->createStagingTexture(description, nvrhi::CpuAccessMode::Read);
			if (!target || !staging)
			{
				throw std::runtime_error("Creating Aster render target or readback staging texture failed");
			}
			m_RenderTarget = std::move(target);
			m_StagingTexture = std::move(staging);
			m_OverlayFramebuffer =
				m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_RenderTarget));
			if (!m_OverlayFramebuffer)
			{
				throw std::runtime_error("Creating overlay output framebuffer failed");
			}
			m_Options.Width = width;
			m_Options.Height = height;
			m_HasRendered = false;
		}

		void CreateSwapchain()
		{
			VkSurfaceCapabilitiesKHR capabilities{};
			CheckVulkan(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &capabilities),
						"Querying surface capabilities");
			if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
			{
				throw std::runtime_error("Window surface does not support transfer destination images");
			}
			uint32_t formatCount = 0;
			CheckVulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr),
						"Querying surface formats");
			std::vector<VkSurfaceFormatKHR> formats(formatCount);
			CheckVulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data()),
						"Reading surface formats");
			const auto format = std::find_if(formats.begin(), formats.end(),
											 [](const auto& candidate)
											 {
												 return (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
														 candidate.format == VK_FORMAT_R8G8B8A8_UNORM) &&
														candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
											 });
			if (format == formats.end())
			{
				throw std::runtime_error("Window surface has no supported RGBA8 or BGRA8 UNORM format");
			}
			m_Format =
				format->format == VK_FORMAT_B8G8R8A8_UNORM ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
			VkExtent2D extent = capabilities.currentExtent;
			if (extent.width == std::numeric_limits<uint32_t>::max())
			{
				int width = 0;
				int height = 0;
				glfwGetFramebufferSize(m_Window, &width, &height);
				extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 1)), capabilities.minImageExtent.width,
										  capabilities.maxImageExtent.width);
				extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 1)),
										   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
			}
			if (extent.width == 0 || extent.height == 0)
			{
				throw std::runtime_error("Cannot create a swapchain for a minimized surface");
			}
			uint32_t imageCount = capabilities.minImageCount + 1;
			if (capabilities.maxImageCount > 0)
			{
				imageCount = std::min(imageCount, capabilities.maxImageCount);
			}
			VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			for (const auto candidate :
				 {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
				  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
			{
				if ((capabilities.supportedCompositeAlpha & candidate) != 0)
				{
					composite = candidate;
					break;
				}
			}
			VkSwapchainCreateInfoKHR create{};
			create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			create.surface = m_Surface;
			create.minImageCount = imageCount;
			create.imageFormat = format->format;
			create.imageColorSpace = format->colorSpace;
			create.imageExtent = extent;
			create.imageArrayLayers = 1;
			create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			create.preTransform = capabilities.currentTransform;
			create.compositeAlpha = composite;
			create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			create.clipped = VK_TRUE;
			CheckVulkan(vkCreateSwapchainKHR(m_Device, &create, nullptr, &m_Swapchain), "Creating Vulkan swapchain");
			CheckVulkan(vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, nullptr),
						"Querying swapchain images");
			std::vector<VkImage> images(imageCount);
			CheckVulkan(vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, images.data()),
						"Reading swapchain images");
			VkSemaphoreCreateInfo semaphore{};
			semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			CheckVulkan(vkCreateSemaphore(m_Device, &semaphore, nullptr, &m_ImageAvailable),
						"Creating image acquisition semaphore");
			m_RenderFinished.resize(imageCount, VK_NULL_HANDLE);
			for (uint32_t index = 0; index < imageCount; ++index)
			{
				nvrhi::TextureDesc description;
				description.width = extent.width;
				description.height = extent.height;
				description.format = m_Format;
				description.isShaderResource = false;
				description.debugName = "Aster swapchain image";
				auto texture = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
																		   nvrhi::Object(images[index]), description);
				if (!texture)
				{
					throw std::runtime_error("NVRHI failed to wrap a swapchain image");
				}
				m_SwapchainTextures.push_back(std::move(texture));
				CheckVulkan(vkCreateSemaphore(m_Device, &semaphore, nullptr, &m_RenderFinished[index]),
							"Creating presentation semaphore");
			}
			m_Options.Width = extent.width;
			m_Options.Height = extent.height;
		}

		void DestroySwapchain() noexcept
		{
			m_SwapchainTextures.clear();
			for (const auto semaphore : m_RenderFinished)
			{
				vkDestroySemaphore(m_Device, semaphore, nullptr);
			}
			m_RenderFinished.clear();
			if (m_ImageAvailable)
			{
				vkDestroySemaphore(m_Device, m_ImageAvailable, nullptr);
				m_ImageAvailable = VK_NULL_HANDLE;
			}
			if (m_Swapchain)
			{
				vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
				m_Swapchain = VK_NULL_HANDLE;
			}
		}

		void RecreateSwapchain()
		{
			WaitForIdle();
			m_NvrhiDevice->runGarbageCollection();
			DestroySwapchain();
			CreateSwapchain();
			CreateRenderTarget(m_Options.Width, m_Options.Height);
		}

		RendererOptions m_Options;
		std::thread::id m_Thread;
		bool m_HasGlfw = false;
		bool m_HasDeviceLease = false;
		bool m_HasRendered = false;
		bool m_InFrame = false;
		InputState m_Input;
		GLFWwindow* m_Window = nullptr;
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_Queue = VK_NULL_HANDLE;
		uint32_t m_QueueFamily = 0;
		uint32_t m_MaxImageDimension = 0;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		VkSemaphore m_ImageAvailable = VK_NULL_HANDLE;
		std::vector<VkSemaphore> m_RenderFinished;
		std::vector<const char*> m_InstanceExtensions;
		std::vector<const char*> m_DeviceExtensions;
		nvrhi::Format m_Format = nvrhi::Format::RGBA8_UNORM;
		nvrhi::vulkan::DeviceHandle m_VulkanDevice;
		nvrhi::DeviceHandle m_NvrhiDevice;
		nvrhi::CommandListHandle m_CommandList;
		nvrhi::TextureHandle m_RenderTarget;
		nvrhi::StagingTextureHandle m_StagingTexture;
		nvrhi::FramebufferHandle m_OverlayFramebuffer;
		OverlayCallback m_OverlayCallback;
		std::unique_ptr<SceneRenderPass> m_ScenePass;
		std::vector<nvrhi::TextureHandle> m_SwapchainTextures;
		std::string m_DeviceName;
		mutable std::mutex m_MessageMutex;
		std::vector<std::string> m_Messages;
		std::atomic<bool> m_HasError = false;
		std::atomic<bool> m_LostMessage = false;
	};

	Renderer::Renderer(const RendererOptions& options) : m_Impl(std::make_unique<Impl>(options))
	{
		m_Impl->Initialize();
	}

	Renderer::~Renderer() = default;

	void Renderer::RenderFrame(const std::array<float, 4>& clearColor)
	{
		m_Impl->RenderFrame(clearColor);
	}

	void Renderer::RenderScene(const Scene& scene, const AssetImporter& importer, const RenderSettings& settings)
	{
		m_Impl->RenderFrame({0, 0, 0, 1}, &scene, &importer, settings);
	}

	void Renderer::InvalidateAssets()
	{
		m_Impl->InvalidateAssets();
	}

	void Renderer::SetEnvironment(const EnvironmentMaps& environment)
	{
		m_Impl->SetEnvironment(environment);
	}

	void Renderer::SetOverlayCallback(OverlayCallback callback)
	{
		m_Impl->SetOverlayCallback(std::move(callback));
	}

	void* Renderer::GetNativeWindow() const
	{
		return m_Impl->GetNativeWindow();
	}

	std::pair<glm::mat4, glm::mat4> Renderer::GetSceneCamera(const Scene& scene) const
	{
		m_Impl->CheckThread();
		return CalculateSceneCamera(scene, m_Impl->GetWidth(), m_Impl->GetHeight());
	}

	RenderImage Renderer::ReadbackRgba8()
	{
		return m_Impl->ReadbackRgba8();
	}

	void Renderer::Resize(uint32_t width, uint32_t height)
	{
		m_Impl->Resize(width, height);
	}

	void Renderer::PollEvents()
	{
		m_Impl->PollEvents();
	}

	InputSnapshot Renderer::GetInputSnapshot() const
	{
		return m_Impl->GetInputSnapshot();
	}

	bool Renderer::ShouldClose() const
	{
		return m_Impl->ShouldClose();
	}

	std::string Renderer::GetDeviceName() const
	{
		return m_Impl->GetDeviceName();
	}

	std::vector<std::string> Renderer::GetValidationMessages() const
	{
		return m_Impl->GetValidationMessages();
	}

	uint32_t Renderer::GetWidth() const
	{
		return m_Impl->GetWidth();
	}

	uint32_t Renderer::GetHeight() const
	{
		return m_Impl->GetHeight();
	}

	void Renderer::Shutdown()
	{
		m_Impl->CheckThread();
		m_Impl->CheckOutsideFrame();
		m_Impl->Shutdown();
	}
} // namespace Aster
