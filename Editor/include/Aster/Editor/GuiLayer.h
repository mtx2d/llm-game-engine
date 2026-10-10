#pragma once

#include <Aster/Renderer/Renderer.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Aster
{
	class CommandProcessor;

	// Builds editor UI before Renderer::RenderScene. Renderer and commands must
	// outlive this layer. All authored edits use the shared command interface.
	class GuiLayer
	{
	  public:
		GuiLayer(Renderer& renderer, CommandProcessor& commands, std::filesystem::path projectRoot,
				 std::optional<std::filesystem::path> scenePath = std::nullopt);
		~GuiLayer();
		GuiLayer(const GuiLayer&) = delete;
		GuiLayer& operator=(const GuiLayer&) = delete;
		void RunFrame();
		[[nodiscard]] RenderSettings GetRenderSettings() const;
		void SetStatus(std::string status);
		void ShowProjectLauncher();

	  private:
		class Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace Aster
