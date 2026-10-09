#include <Aster/Input/InputState.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Aster
{
	namespace
	{
		bool Finite(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		glm::vec2 Vector(double x, double y)
		{
			if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
				std::abs(y) > std::numeric_limits<float>::max())
			{
				throw std::invalid_argument("Input coordinates must be finite floats");
			}
			return {static_cast<float>(x), static_cast<float>(y)};
		}
	} // namespace

	bool InputState::IsValidKey(std::string_view key)
	{
		if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9')))
		{
			return true;
		}
		if (key.starts_with('F') && key.size() >= 2 && key.size() <= 3 && key[1] != '0')
		{
			int functionKey = 0;
			const auto parsed = std::from_chars(key.data() + 1, key.data() + key.size(), functionKey);
			if (parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size() && functionKey >= 1 &&
				functionKey <= 25)
			{
				return true;
			}
		}
		if (key.starts_with("Keypad") && key.size() == 7 && key[6] >= '0' && key[6] <= '9')
		{
			return true;
		}
		constexpr std::string_view names[] = {
			"Space",		  "Escape",		 "Enter",		  "Tab",		  "Backspace",
			"Insert",		  "Delete",		 "Left",		  "Right",		  "Up",
			"Down",			  "PageUp",		 "PageDown",	  "Home",		  "End",
			"CapsLock",		  "ScrollLock",	 "NumLock",		  "PrintScreen",  "Pause",
			"LeftShift",	  "RightShift",	 "LeftControl",	  "RightControl", "LeftAlt",
			"RightAlt",		  "LeftSuper",	 "RightSuper",	  "Menu",		  "Apostrophe",
			"Comma",		  "Minus",		 "Period",		  "Slash",		  "Semicolon",
			"Equal",		  "LeftBracket", "Backslash",	  "RightBracket", "GraveAccent",
			"World1",		  "World2",		 "KeypadDecimal", "KeypadDivide", "KeypadMultiply",
			"KeypadSubtract", "KeypadAdd",	 "KeypadEnter",	  "KeypadEqual"};
		return std::find(std::begin(names), std::end(names), key) != std::end(names);
	}

	std::size_t InputState::MouseButtonIndex(std::string_view button)
	{
		constexpr std::array<std::string_view, 8> names = {"Left",	  "Right",	 "Middle",	"Back",
														   "Forward", "Button6", "Button7", "Button8"};
		const auto found = std::find(names.begin(), names.end(), button);
		if (found == names.end())
		{
			throw std::invalid_argument("Unsupported mouse button: " + std::string(button));
		}
		return static_cast<std::size_t>(std::distance(names.begin(), found));
	}

	void InputState::BeginFrame()
	{
		m_Snapshot.KeysPressed.clear();
		m_Snapshot.KeysReleased.clear();
		m_Snapshot.MousePressed.fill(false);
		m_Snapshot.MouseReleased.fill(false);
		m_Snapshot.MouseDelta = {0, 0};
		m_Snapshot.Wheel = {0, 0};
	}

	void InputState::KeyEvent(std::string_view key, bool down)
	{
		if (!IsValidKey(key))
		{
			throw std::invalid_argument("Unsupported key: " + std::string(key));
		}
		const std::string name(key);
		if (down && m_Snapshot.Focused)
		{
			if (m_Snapshot.KeysDown.insert(name).second)
			{
				m_Snapshot.KeysPressed.insert(name);
			}
		}
		else if (m_Snapshot.KeysDown.erase(name) != 0)
		{
			m_Snapshot.KeysReleased.insert(name);
		}
	}

	void InputState::MouseButtonEvent(std::size_t button, bool down)
	{
		if (button >= m_Snapshot.MouseDown.size())
		{
			throw std::invalid_argument("Mouse button index must be in [0, 7]");
		}
		down = down && m_Snapshot.Focused;
		if (down && !m_Snapshot.MouseDown[button])
		{
			m_Snapshot.MousePressed[button] = true;
		}
		if (!down && m_Snapshot.MouseDown[button])
		{
			m_Snapshot.MouseReleased[button] = true;
		}
		m_Snapshot.MouseDown[button] = down;
	}

	void InputState::CursorEvent(double x, double y)
	{
		const auto position = Vector(x, y);
		const auto delta =
			m_HasCursorPosition && m_Snapshot.Focused ? position - m_Snapshot.MousePosition : glm::vec2(0);
		const auto total = m_Snapshot.MouseDelta + delta;
		if (!Finite(total))
		{
			throw std::invalid_argument("Accumulated mouse delta exceeds finite range");
		}
		m_Snapshot.MouseDelta = total;
		m_Snapshot.MousePosition = position;
		m_HasCursorPosition = true;
	}

	void InputState::ScrollEvent(double x, double y)
	{
		const auto delta = Vector(x, y);
		if (!m_Snapshot.Focused)
		{
			return;
		}
		const auto total = m_Snapshot.Wheel + delta;
		if (!Finite(total))
		{
			throw std::invalid_argument("Accumulated mouse wheel exceeds finite range");
		}
		m_Snapshot.Wheel = total;
	}

	void InputState::FocusEvent(bool focused)
	{
		m_Snapshot.Focused = focused;
		if (!focused)
		{
			m_Snapshot.KeysReleased.insert(m_Snapshot.KeysDown.begin(), m_Snapshot.KeysDown.end());
			m_Snapshot.KeysDown.clear();
			for (std::size_t index = 0; index < m_Snapshot.MouseDown.size(); ++index)
			{
				m_Snapshot.MouseReleased[index] = m_Snapshot.MouseReleased[index] || m_Snapshot.MouseDown[index];
				m_Snapshot.MouseDown[index] = false;
			}
			m_Snapshot.MouseDelta = {0, 0};
			m_Snapshot.Wheel = {0, 0};
		}
		m_HasCursorPosition = false;
	}

	const InputSnapshot& InputState::GetSnapshot() const
	{
		return m_Snapshot;
	}

	void InputState::Validate(const InputSnapshot& snapshot)
	{
		for (const auto* keys : {&snapshot.KeysDown, &snapshot.KeysPressed, &snapshot.KeysReleased})
		{
			for (const auto& key : *keys)
			{
				if (!IsValidKey(key))
				{
					throw std::invalid_argument("Unsupported key: " + key);
				}
			}
		}
		if (!Finite(snapshot.MousePosition) || !Finite(snapshot.MouseDelta) || !Finite(snapshot.Wheel))
		{
			throw std::invalid_argument("Input vectors must be finite");
		}
		if (!snapshot.Focused &&
			(!snapshot.KeysDown.empty() ||
			 std::any_of(snapshot.MouseDown.begin(), snapshot.MouseDown.end(), [](bool down) { return down; })))
		{
			throw std::invalid_argument("Unfocused input cannot have held keys or mouse buttons");
		}
		for (const auto& key : snapshot.KeysPressed)
		{
			if (!snapshot.KeysDown.contains(key) && !snapshot.KeysReleased.contains(key))
			{
				throw std::invalid_argument("Pressed key must be held or released in the same snapshot: " + key);
			}
		}
		for (const auto& key : snapshot.KeysReleased)
		{
			if (snapshot.KeysDown.contains(key) && !snapshot.KeysPressed.contains(key))
			{
				throw std::invalid_argument("Released key cannot remain held without a new press: " + key);
			}
		}
		for (std::size_t index = 0; index < snapshot.MouseDown.size(); ++index)
		{
			if ((snapshot.MousePressed[index] && !snapshot.MouseDown[index] && !snapshot.MouseReleased[index]) ||
				(snapshot.MouseReleased[index] && snapshot.MouseDown[index] && !snapshot.MousePressed[index]))
			{
				throw std::invalid_argument("Contradictory mouse button transition");
			}
		}
	}
} // namespace Aster
