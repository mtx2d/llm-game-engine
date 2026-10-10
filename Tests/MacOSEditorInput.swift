import AppKit
import ApplicationServices
import Darwin
import Foundation

@MainActor
final class EditorInput
{
	let game: GameWindow
	let client: CGRect
	let source: CGEventSource
	var pointer: CGPoint
	var leftDown = false
	var commandDown = false
	var keyDown: CGKeyCode?
	var eventsPosted = 0
	var mouseEventNumber = 0

	init(game: GameWindow) throws
	{
		self.game = game
		try game.CheckFocused()
		guard let window = game.window, let source = CGEventSource(stateID: .privateState) else
		{
			throw TestFailure(description: "Cannot initialize owned editor input")
		}
		self.source = source
		// GLFW's decorated/resizable Cocoa window uses these standard style bits.
		let outer = CGRect(origin: .zero, size: window.frame.size)
		let content = NSWindow.contentRect(forFrameRect: outer,
			styleMask: [.titled, .closable, .miniaturizable, .resizable])
		let client = CGRect(x: content.minX, y: outer.height - content.maxY, width: content.width, height: content.height)
		self.client = client
		try Require(client.width >= 900 && client.height >= 600, "Native editor client area is too small")
		pointer = CGPoint(x: window.frame.minX + client.midX, y: window.frame.minY + client.midY)
	}

	func Settle() async throws
	{
		// Separate native event edges so ImGui can observe held controls. This
		// pacing is not a rendering acknowledgment: the shared workflow proves
		// completion through persisted scene predicates and owned-window pixels.
		// ScreenCaptureKit emits idle callbacks for an unchanged display, so a
		// responsive static editor cannot promise new complete frames per event.
		try await Task.sleep(nanoseconds: 250_000_000)
		try game.CheckFocused()
	}

	func GuardInput() throws
	{
		try CheckPermissions(Permissions())
		try game.CheckFocused()
		let modifiers: CGEventFlags = [.maskShift, .maskControl, .maskAlternate, .maskCommand]
		let allowed: CGEventFlags = commandDown ? .maskCommand : []
		try Require(CGEventSource.flagsState(.combinedSessionState).intersection(modifiers).subtracting(allowed).isEmpty &&
			(!CGEventSource.buttonState(.combinedSessionState, button: .left) || leftDown) &&
			!CGEventSource.buttonState(.combinedSessionState, button: .right) &&
			!CGEventSource.buttonState(.combinedSessionState, button: .center),
			"Existing held controls prevent isolated native editor input")
	}

	func CursorPosition() throws -> CGPoint
	{
		guard let event = CGEvent(source: nil) else
		{
			throw TestFailure(description: "Cannot read the native cursor position")
		}
		return event.location
	}

	func RequireCursor(at point: CGPoint) throws -> CGPoint
	{
		let actual = try CursorPosition()
		try Require(abs(actual.x - point.x) <= 1 && abs(actual.y - point.y) <= 1,
			"Native cursor did not remain at its owned-client target: expected \(point), observed \(actual)")
		return actual
	}

	func RequireOwnedPoint() throws -> UInt32
	{
		try game.CheckFocused()
		guard let window = game.window, let ownedWindow = game.accessibilityWindow else
		{
			throw TestFailure(description: "No owned editor window for pointer hit-testing")
		}
		// A system-wide hit test uses the actual screen z-order. Restricting this
		// query to our application could overlook a different app covering it.
		var hit: AXUIElement?
		try RequireAX(AXUIElementCopyElementAtPosition(AXUIElementCreateSystemWide(),
			Float(pointer.x), Float(pointer.y), &hit), "Hit-test native pointer target")
		guard let hit else { throw TestFailure(description: "Native pointer target has no accessibility element") }
		try Require(try Owner(hit) == game.pid, "Native pointer target is covered by another process")
		let hitWindow: AXUIElement
		if CFEqual(hit, ownedWindow) { hitWindow = hit }
		else { hitWindow = try ElementAttribute(hit, kAXWindowAttribute) }
		try Require(try Owner(hitWindow) == game.pid && CFEqual(hitWindow, ownedWindow),
			"Native pointer target belongs to another window")
		return window.windowID
	}

	func MakeMouse(_ type: CGEventType) throws -> CGEvent
	{
		guard let event = CGEvent(mouseEventSource: source, mouseType: type,
			mouseCursorPosition: pointer, mouseButton: .left) else
		{
			throw TestFailure(description: "Cannot create a native editor mouse event")
		}
		event.flags = []
		event.setIntegerValueField(.mouseEventClickState, value: 1)
		event.setIntegerValueField(.mouseEventNumber, value: Int64(mouseEventNumber))
		event.setDoubleValueField(.mouseEventPressure, value: type == .leftMouseDown || type == .leftMouseDragged ? 1 : 0)
		return event
	}

	func Mouse(_ action: String, x: Double, y: Double) async throws -> [String: Any]
	{
		try GuardInput()
		try Require(x.isFinite && y.isFinite && x >= 0 && y >= 0 && x < Double(client.width) && y < Double(client.height),
			"Native pointer position lies outside the owned editor client")
		guard let window = game.window else { throw TestFailure(description: "No owned editor window") }
		pointer = CGPoint(x: window.frame.minX + client.minX + CGFloat(x),
			y: window.frame.minY + client.minY + CGFloat(y))
		let type: CGEventType
		switch action
		{
		case "move": type = leftDown ? .leftMouseDragged : .mouseMoved
		case "down":
			try Require(!leftDown, "Native mouse button is already down")
			mouseEventNumber += 1
			type = .leftMouseDown
		case "up":
			try Require(leftDown, "Native mouse button was not pressed")
			type = .leftMouseUp
		default: throw TestFailure(description: "Unknown native mouse operation")
		}
		// ImGui's GLFW fallback polls Cocoa mouseLocationOutsideOfEventStream.
		// Place and check the physical cursor before native WindowServer routing.
		let before = try CursorPosition()
		let warp = CGWarpMouseCursorPosition(pointer)
		try Require(warp == .success, "Cannot move the native cursor to its owned-client target: \(warp.rawValue)")
		_ = try RequireCursor(at: pointer)
		let hitWindow = try RequireOwnedPoint()
		let event = try MakeMouse(type)
		// Mouse events enter the ordinary foreground HID path only after the
		// focus, actual cursor and topmost owned-window checks all succeed.
		event.post(tap: .cghidEventTap)
		eventsPosted += 1
		if action == "down" { leftDown = true }
		if action == "up" { leftDown = false }
		try await Settle()
		let after = try RequireCursor(at: pointer)
		_ = try RequireOwnedPoint()
		return ["before_x": Double(before.x), "before_y": Double(before.y),
			"target_x": Double(pointer.x), "target_y": Double(pointer.y),
			"after_x": Double(after.x), "after_y": Double(after.y), "hit_window_id": hitWindow,
			"posting_scope": "foreground_hid", "client_x": x, "client_y": y,
			"mouse_event_number": mouseEventNumber]
	}

	func Key(_ key: String, down: Bool) async throws
	{
		try GuardInput()
		let code: CGKeyCode
		switch key
		{
		case "Command": code = 55
		case "A": code = 0
		case "Return": code = 36
		default: throw TestFailure(description: "Unsupported native editor key")
		}
		if down
		{
			try Require(!CGEventSource.keyState(.combinedSessionState, key: code),
				"A physical key is already held before native editor input")
		}
		if key == "Command"
		{
			try Require(commandDown != down, "Invalid Command key transition")
		}
		else
		{
			try Require(down ? keyDown == nil : keyDown == code, "Invalid native key transition")
		}
		guard let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else
		{
			throw TestFailure(description: "Cannot create native editor keyboard event")
		}
		if key == "Command" { event.type = .flagsChanged }
		let nextCommandDown = key == "Command" ? down : commandDown
		event.flags = nextCommandDown ? .maskCommand : []
		event.postToPid(game.pid)
		eventsPosted += 1
		if key == "Command" { commandDown = down }
		else { keyDown = down ? code : nil }
		try await Settle()
	}

	func Text(_ text: String) async throws
	{
		try GuardInput()
		try Require(!commandDown && keyDown == nil && !leftDown && !text.isEmpty && text.utf16.count <= 256,
			"Invalid native text-input state or length")
		for character in text
		{
			try GuardInput()
			try Require(!CGEventSource.keyState(.combinedSessionState, key: 0),
				"A physical key is held during native text input")
			let units = Array(String(character).utf16)
			guard let down = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: true),
				let up = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: false) else
			{
				throw TestFailure(description: "Cannot create native Unicode input")
			}
			down.flags = []
			up.flags = []
			units.withUnsafeBufferPointer
			{
				down.keyboardSetUnicodeString(stringLength: units.count, unicodeString: $0.baseAddress)
				up.keyboardSetUnicodeString(stringLength: units.count, unicodeString: $0.baseAddress)
			}
			down.postToPid(game.pid)
			eventsPosted += 1
			keyDown = 0
			try await Settle()
			up.postToPid(game.pid)
			eventsPosted += 1
			keyDown = nil
			try await Settle()
		}
	}

	func ReleaseHeldInput() throws
	{
		try game.CheckAlive()
		// Release only controls pressed by this helper. A foreground mouse release
		// requires the same focused owned point; keyboard releases remain PID-only.
		if leftDown
		{
			try GuardInput()
			_ = try RequireCursor(at: pointer)
			_ = try RequireOwnedPoint()
			let event = try MakeMouse(.leftMouseUp)
			event.post(tap: .cghidEventTap)
			leftDown = false
		}
		for code in [keyDown, commandDown ? CGKeyCode(55) : nil].compactMap({ $0 })
		{
			guard let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: false) else
			{
				throw TestFailure(description: "Cannot release owned keyboard key")
			}
			if code == 55 { event.type = .flagsChanged }
			event.flags = []
			event.postToPid(game.pid)
		}
		keyDown = nil
		commandDown = false
	}
}

@main
struct NativeEditorInput
{
	@MainActor
	static func main() async
	{
		var input: EditorInput?
		do
		{
			let arguments = Array(CommandLine.arguments.dropFirst())
			if arguments.count == 3 && arguments[0] == "--check-process"
			{
				guard let pid = Int32(arguments[1]) else { throw TestFailure(description: "Invalid launched PID") }
				let identity = try ProcessIdentity(pid: pid, executable: URL(fileURLWithPath: arguments[2]))
				try WriteJSON(["ok": true, "identity_ready": true])
				let request = await Task.detached(operation: { readLine() }).value
				try Require(request == "check", "Expected identity check")
				try identity.CheckAlive()
				try WriteJSON(["ok": true, "identity_checked": true])
				return
			}
			let permissions = Permissions()
			if arguments == ["--preflight"]
			{
				try WriteJSON(permissions)
				try CheckPermissions(permissions)
				return
			}
			try CheckPermissions(permissions)
			try Require(arguments.count == 3, "Expected launched PID, executable and artifact directory")
			guard let pid = Int32(arguments[0]) else { throw TestFailure(description: "Invalid launched PID") }
			let game = try GameWindow(pid: pid, executable: URL(fileURLWithPath: arguments[1]))
			try await game.FindAndFocus()
			let driver = try EditorInput(game: game)
			input = driver
			let ready = try await game.WaitForImage(region: driver.client)
			try ready.Save(URL(fileURLWithPath: arguments[2]).appendingPathComponent("EditorReady.ppm"))
			try WriteJSON(["ok": true, "pid": pid, "width": Int(driver.client.width.rounded()),
				"height": Int(driver.client.height.rounded()), "window_id": game.window!.windowID,
				"permissions": permissions] as [String: Any])
			while let line = await Task.detached(operation: { readLine() }).value
			{
				try Require(line.utf8.count <= 4096, "Native editor request exceeds its bound")
				guard let request = try JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
					let operation = request["operation"] as? String else
				{
					throw TestFailure(description: "Invalid native editor request")
				}
				var response: [String: Any] = ["ok": true]
				switch operation
				{
				case "mouse":
					guard let action = request["action"] as? String, let x = request["x"] as? Double,
						let y = request["y"] as? Double else { throw TestFailure(description: "Invalid pointer request") }
					response["cursor"] = try await driver.Mouse(action, x: x, y: y)
				case "key":
					guard let key = request["key"] as? String, let down = request["down"] as? Bool else
					{
						throw TestFailure(description: "Invalid keyboard request")
					}
					try await driver.Key(key, down: down)
				case "text":
					guard let text = request["text"] as? String else { throw TestFailure(description: "Invalid text request") }
					try await driver.Text(text)
				case "capture":
					let frame = try await game.Capture(region: driver.client)
					try frame.Save(URL(fileURLWithPath: arguments[2]).appendingPathComponent("EditorSnapshot.ppm"))
				case "requestClose":
					try driver.ReleaseHeldInput()
					try game.RequestClose()
					response["requested"] = true
				case "close":
					try driver.ReleaseHeldInput()
					try game.RequestClose()
					try WriteJSON(["ok": true, "closed": true, "events_posted": driver.eventsPosted] as [String: Any])
					return
				default: throw TestFailure(description: "Unknown native editor operation")
				}
				response["events_posted"] = driver.eventsPosted
				try WriteJSON(response)
			}
			throw TestFailure(description: "Native editor driver reached EOF before graceful close")
		}
		catch
		{
			var diagnostic = "Native macOS editor: \(error)\n"
			if let input
			{
				do { try input.ReleaseHeldInput() }
				catch { diagnostic += "Owned input cleanup: \(error)\n" }
			}
			do { try FileHandle.standardError.write(contentsOf: Data(diagnostic.utf8)) }
			catch { exit(2) }
			exit(1)
		}
	}
}
