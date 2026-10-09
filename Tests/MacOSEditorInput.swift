import AppKit
import CoreMedia
import Darwin
import Foundation
import ScreenCaptureKit

// Only new, complete compositor frames acknowledge input. Cursor-only movement
// and idle callbacks must not masquerade as rendered UI progress.
final class PresentedFrames: NSObject, SCStreamOutput, SCStreamDelegate
{
	private let lock = NSLock()
	private var count: UInt64 = 0
	private var failure: String?

	func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType)
	{
		guard type == .screen, sampleBuffer.isValid,
			let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false)
				as? [[SCStreamFrameInfo: Any]],
			let status = attachments.first?[.status] as? Int,
			SCFrameStatus(rawValue: status) == .complete else { return }
		lock.lock()
		count += 1
		lock.unlock()
	}

	func stream(_ stream: SCStream, didStopWithError error: Error)
	{
		lock.lock()
		failure = String(describing: error)
		lock.unlock()
	}

	func Snapshot() throws -> UInt64
	{
		lock.lock()
		defer { lock.unlock() }
		if let failure { throw TestFailure(description: "Owned-window frame stream failed: \(failure)") }
		return count
	}
}

@MainActor
final class EditorInput
{
	let game: GameWindow
	let frames: PresentedFrames
	let stream: SCStream
	let client: CGRect
	let source: CGEventSource
	var pointer: CGPoint
	var leftDown = false
	var commandDown = false
	var keyDown: CGKeyCode?

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
		let configuration = SCStreamConfiguration()
		configuration.width = Int(window.frame.width.rounded())
		configuration.height = Int(window.frame.height.rounded())
		configuration.minimumFrameInterval = CMTime(value: 1, timescale: 60)
		configuration.queueDepth = 3
		configuration.showsCursor = false
		configuration.ignoreShadowsSingleWindow = true
		let frames = PresentedFrames()
		self.frames = frames
		stream = SCStream(filter: SCContentFilter(desktopIndependentWindow: window),
			configuration: configuration, delegate: frames)
		try stream.addStreamOutput(frames, type: .screen,
			sampleHandlerQueue: DispatchQueue(label: "Aster.NativeEditorFrames"))
	}

	func Wait(after count: UInt64, timeout: Double = 15) async throws
	{
		let deadline = ProcessInfo.processInfo.systemUptime + timeout
		while try frames.Snapshot() < count + 3
		{
			try game.CheckAlive()
			try Require(ProcessInfo.processInfo.systemUptime < deadline,
				"Editor did not present three new complete frames after native input")
			try await Task.sleep(nanoseconds: 10_000_000)
		}
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

	func Mouse(_ action: String, x: Double, y: Double) async throws
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
			type = .leftMouseDown
		case "up":
			try Require(leftDown, "Native mouse button was not pressed")
			type = .leftMouseUp
		default: throw TestFailure(description: "Unknown native mouse operation")
		}
		guard let event = CGEvent(mouseEventSource: source, mouseType: type,
			mouseCursorPosition: pointer, mouseButton: .left) else
		{
			throw TestFailure(description: "Cannot create native editor pointer event")
		}
		event.flags = []
		event.setIntegerValueField(.mouseEventClickState, value: 1)
		let before = try frames.Snapshot()
		event.postToPid(game.pid)
		if action == "down" { leftDown = true }
		if action == "up" { leftDown = false }
		try await Wait(after: before)
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
		let before = try frames.Snapshot()
		event.postToPid(game.pid)
		if key == "Command" { commandDown = down }
		else { keyDown = down ? code : nil }
		try await Wait(after: before)
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
			var before = try frames.Snapshot()
			down.postToPid(game.pid)
			keyDown = 0
			try await Wait(after: before)
			before = try frames.Snapshot()
			up.postToPid(game.pid)
			keyDown = nil
			try await Wait(after: before)
		}
	}

	func ReleaseHeldInput() throws
	{
		try game.CheckAlive()
		// Cleanup releases target only the PID that received the original presses.
		// They never activate a different app or enter the global event stream.
		if leftDown
		{
			guard let event = CGEvent(mouseEventSource: source, mouseType: .leftMouseUp,
				mouseCursorPosition: pointer, mouseButton: .left) else
			{
				throw TestFailure(description: "Cannot release owned mouse button")
			}
			event.postToPid(game.pid)
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
			try await driver.stream.startCapture()
			try await driver.Wait(after: 0, timeout: 30)
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
				switch operation
				{
				case "mouse":
					guard let action = request["action"] as? String, let x = request["x"] as? Double,
						let y = request["y"] as? Double else { throw TestFailure(description: "Invalid pointer request") }
					try await driver.Mouse(action, x: x, y: y)
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
				case "close":
					try driver.ReleaseHeldInput()
					try await driver.stream.stopCapture()
					try game.RequestClose()
					try WriteJSON(["ok": true, "closed": true, "complete_frames": try driver.frames.Snapshot()] as [String: Any])
					return
				default: throw TestFailure(description: "Unknown native editor operation")
				}
				try WriteJSON(["ok": true, "complete_frames": try driver.frames.Snapshot()] as [String: Any])
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
				do { try await input.stream.stopCapture() }
				catch { diagnostic += "Capture cleanup: \(error)\n" }
			}
			do { try FileHandle.standardError.write(contentsOf: Data(diagnostic.utf8)) }
			catch { exit(2) }
			exit(1)
		}
	}
}
