#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>

namespace Aster
{
	struct InputSnapshot
	{
		// Keys are case-sensitive: A-Z, 0-9, F1-F25, arrows Left/Right/Up/Down,
		// Space/Enter/Escape/Tab/Backspace/Insert/Delete/Home/End/PageUp/PageDown,
		// Left/Right Shift, Control, Alt, Super (e.g. LeftShift), Menu, lock keys,
		// PrintScreen/Pause, named punctuation, and Keypad0-Keypad9/operators.
		std::set<std::string> KeysDown;
		std::set<std::string> KeysPressed;
		std::set<std::string> KeysReleased;
		// Left, Right, Middle, Back, Forward, Button6, Button7, Button8.
		std::array<bool, 8> MouseDown{};
		std::array<bool, 8> MousePressed{};
		std::array<bool, 8> MouseReleased{};
		// Window content coordinates, origin at the top left. The first cursor
		// sample and the first sample after focus changes produce no delta.
		glm::vec2 MousePosition{0.0f};
		glm::vec2 MouseDelta{0.0f};
		// Horizontal and vertical scroll units, independent of cursor coordinates.
		glm::vec2 Wheel{0.0f};
		bool Focused = true;
	};

	// Window-independent event accumulator. Call BeginFrame before polling events,
	// then send its snapshot to Simulation::SetInput. Repeat key events do not
	// produce additional presses. A press followed by release retains both edges.
	// Focus loss releases all held controls and clears motion. All coordinates
	// must be finite; unsupported key/button names and contradictory edges throw.
	class InputState
	{
	  public:
		void BeginFrame();
		void KeyEvent(std::string_view key, bool down);
		void MouseButtonEvent(std::size_t button, bool down);
		void CursorEvent(double x, double y);
		void ScrollEvent(double x, double y);
		void FocusEvent(bool focused);
		[[nodiscard]] const InputSnapshot& GetSnapshot() const;
		// Reject unsupported key/button names and nonfinite/contradictory snapshots.
		static void Validate(const InputSnapshot& snapshot);
		[[nodiscard]] static bool IsValidKey(std::string_view key);
		[[nodiscard]] static std::size_t MouseButtonIndex(std::string_view button);

	  private:
		InputSnapshot m_Snapshot;
		bool m_HasCursorPosition = false;
	};
} // namespace Aster
