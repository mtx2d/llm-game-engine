// Native test driver only; the engine and shipping runtime do not depend on Swift.
// Public API references:
// https://developer.apple.com/documentation/coregraphics/cgevent/posttopid(_:)
// https://developer.apple.com/documentation/screencapturekit/scscreenshotmanager
import AppKit
import ApplicationServices
import CryptoKit
import Darwin
import Foundation
import ScreenCaptureKit

struct TestFailure: Error, CustomStringConvertible
{
	let description: String
}

struct AccessibilityFailure: Error, CustomStringConvertible
{
	let result: AXError
	let operation: String
	var description: String { "\(operation) failed with AX error \(result.rawValue)" }
}

func Require(_ condition: @autoclosure () throws -> Bool, _ message: String) throws
{
	if try condition() == false
	{
		throw TestFailure(description: message)
	}
}

func RequireAX(_ result: AXError, _ operation: String) throws
{
	if result != .success { throw AccessibilityFailure(result: result, operation: operation) }
}

func Attribute(_ element: AXUIElement, _ name: String) throws -> CFTypeRef
{
	var value: CFTypeRef?
	try RequireAX(AXUIElementCopyAttributeValue(element, name as CFString, &value), name)
	guard let value else { throw TestFailure(description: "Missing accessibility attribute \(name)") }
	return value
}

func ElementAttribute(_ element: AXUIElement, _ name: String) throws -> AXUIElement
{
	let value = try Attribute(element, name)
	try Require(CFGetTypeID(value) == AXUIElementGetTypeID(), "\(name) is not an accessibility element")
	return value as! AXUIElement
}

func Owner(_ element: AXUIElement) throws -> pid_t
{
	var pid: pid_t = 0
	try RequireAX(AXUIElementGetPid(element, &pid), "AXUIElementGetPid")
	return pid
}

func Permissions() -> [String: Bool]
{
	// None of these APIs requests permission or changes system settings.
	return ["accessibility": AXIsProcessTrusted(), "post_event": CGPreflightPostEventAccess(),
			"screen_capture": CGPreflightScreenCaptureAccess()]
}

func CheckPermissions(_ permissions: [String: Bool]) throws
{
	let missing = permissions.filter { !$0.value }.map { $0.key }.sorted()
	try Require(missing.isEmpty,
		"Required macOS permissions are absent: \(missing.joined(separator: ", ")). " +
		"No permission prompt or TCC change was attempted; native gameplay is unverified.")
}

func WriteJSON(_ object: Any) throws
{
	var data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
	data.append(0x0A)
	try FileHandle.standardOutput.write(contentsOf: data)
}

struct Frame
{
	let width: Int
	let height: Int
	let pixels: Data
	var digest: String { Data(SHA256.hash(data: pixels)).base64EncodedString() }
	var populated: Bool { Set(pixels).count > 32 }

	func Save(_ path: URL) throws
	{
		var file = Data("P6\n\(width) \(height)\n255\n".utf8)
		file.append(pixels)
		try file.write(to: path)
	}
}

@MainActor
final class GameWindow
{
	let pid: pid_t
	let executable: URL
	let application: AXUIElement
	var window: SCWindow?
	var accessibilityWindow: AXUIElement?
	var focusRetries = 0

	init(pid: pid_t, executable: URL) throws
	{
		try Require(pid > 1 && pid != getpid(), "Refusing an invalid or self PID")
		self.pid = pid
		self.executable = executable.resolvingSymlinksInPath()
		application = AXUIElementCreateApplication(pid)
		// The system-wide element sets the default bound for derived window and
		// close-button AX objects too. It is not used to query foreground focus.
		let timeoutScope = AXUIElementCreateSystemWide()
		try RequireAX(AXUIElementSetMessagingTimeout(timeoutScope, 2), "Set global AX timeout")
		try RequireAX(AXUIElementSetMessagingTimeout(application, 2), "Set application AX timeout")
	}

	func CheckAlive() throws
	{
		try Require(kill(pid, 0) == 0, "Launched game exited during native interaction")
		guard let app = NSRunningApplication(processIdentifier: pid), let path = app.executableURL else
		{
			throw TestFailure(description: "Launched PID has no running application executable")
		}
		try Require(!app.isTerminated && path.resolvingSymlinksInPath() == executable,
			"PID no longer belongs to the launched game executable")
	}

	func FindAndFocus() async throws
	{
		let deadline = ProcessInfo.processInfo.systemUptime + 30
		while ProcessInfo.processInfo.systemUptime < deadline
		{
			try Require(kill(pid, 0) == 0, "Game exited before creating its native window")
			try CheckPermissions(Permissions())
			let content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: true)
			let candidates = content.windows.filter
			{
				$0.owningApplication?.processID == pid && $0.windowLayer == 0 && $0.isOnScreen &&
				$0.frame.width > 128 && $0.frame.height > 128
			}
			try Require(candidates.count <= 1, "Launched game has multiple eligible windows; refusing ambiguous input")
			if let candidate = candidates.first
			{
				try CheckAlive()
				window = candidate
				let focusDeadline = ProcessInfo.processInfo.systemUptime + 5
				var nextActivationRequest = 0.0
				var lastObservation = "The launched game has not become frontmost"
				while ProcessInfo.processInfo.systemUptime < focusDeadline
				{
					// Let AppKit process activation before reading its foreground state.
					try await Task.sleep(nanoseconds: 20_000_000)
					try CheckAlive()
					if NSWorkspace.shared.frontmostApplication?.processIdentifier != pid
					{
						// A GLFW window can become visible while its app is still busy
						// preparing the first Vulkan frame. Activation is an asynchronous
						// AppKit request; AX focus is proved separately once it completes.
						let now = ProcessInfo.processInfo.systemUptime
						if now >= nextActivationRequest
						{
							guard let running = NSRunningApplication(processIdentifier: pid) else
							{
								throw TestFailure(description: "Launched game disappeared before activation")
							}
							let accepted = running.activate(options: [])
							lastObservation = accepted ? "AppKit accepted activation; waiting for foreground focus" :
								"AppKit has not accepted the launched game's activation request"
							nextActivationRequest = now + 0.25
						}
					}
					else
					{
						do
						{
							let focused = try ElementAttribute(application, kAXFocusedWindowAttribute)
							try Require(try Owner(focused) == pid, "Focused window belongs to another process")
							try MatchWindow(focused, candidate)
							accessibilityWindow = focused
							return
						}
						catch let error as AccessibilityFailure
						{
							// An app can temporarily have no focused window or be unable to
							// answer AX messages while activation/window creation completes.
							guard error.result == .cannotComplete || error.result == .noValue else { throw error }
							lastObservation = error.description
							focusRetries += 1
						}
					}
				}
				throw TestFailure(description:
					"Cannot focus the launched game; no keyboard input was sent. Last observation: \(lastObservation)")
			}
			try await Task.sleep(nanoseconds: 20_000_000)
		}
		throw TestFailure(description: "Launched game did not create a visible owned window")
	}

	func MatchWindow(_ element: AXUIElement, _ candidate: SCWindow) throws
	{
		let pointValue = try Attribute(element, kAXPositionAttribute)
		let sizeValue = try Attribute(element, kAXSizeAttribute)
		try Require(CFGetTypeID(pointValue) == AXValueGetTypeID() && CFGetTypeID(sizeValue) == AXValueGetTypeID(),
			"Focused window has invalid accessibility geometry")
		var point = CGPoint.zero
		var size = CGSize.zero
		try Require(AXValueGetValue(pointValue as! AXValue, .cgPoint, &point) &&
			AXValueGetValue(sizeValue as! AXValue, .cgSize, &size), "Cannot read window geometry")
		try Require(abs(point.x - candidate.frame.minX) <= 1 && abs(point.y - candidate.frame.minY) <= 1 &&
			abs(size.width - candidate.frame.width) <= 1 && abs(size.height - candidate.frame.height) <= 1,
			"Focused accessibility window does not match the captured owned window")
	}

	func CheckFocused() throws
	{
		try CheckAlive()
		guard let window, let accessibilityWindow else { throw TestFailure(description: "No selected game window") }
		// AppKit defines frontmostApplication as the application receiving key
		// events. AX separately proves which owned window holds keyboard focus.
		try Require(NSWorkspace.shared.frontmostApplication?.processIdentifier == pid, "Game lost foreground focus")
		let focusedWindow = try ElementAttribute(application, kAXFocusedWindowAttribute)
		try Require(try Owner(focusedWindow) == pid, "Keyboard focus belongs to another process")
		try Require(CFEqual(focusedWindow, accessibilityWindow), "Game keyboard focus moved to another window")
		let options: CGWindowListOption = [.optionIncludingWindow]
		guard let information = CGWindowListCopyWindowInfo(options, window.windowID) as? [[String: Any]],
			information.count == 1,
			let owner = information[0][kCGWindowOwnerPID as String] as? NSNumber,
			let visible = information[0][kCGWindowIsOnscreen as String] as? Bool else
		{
			throw TestFailure(description: "Captured window disappeared")
		}
		try Require(owner.int32Value == pid && visible, "Captured window is not visible and owned by the launched PID")
		try MatchWindow(focusedWindow, window)
	}

	func Space() throws
	{
		try CheckPermissions(Permissions())
		try CheckFocused()
		let modifiers: CGEventFlags = [.maskShift, .maskControl, .maskAlternate, .maskCommand]
		try Require(CGEventSource.flagsState(.combinedSessionState).intersection(modifiers).isEmpty &&
			!CGEventSource.keyState(.combinedSessionState, key: 49), "Existing held keys prevent isolated Space input")
		guard let source = CGEventSource(stateID: .privateState),
			let down = CGEvent(keyboardEventSource: source, virtualKey: 49, keyDown: true),
			let up = CGEvent(keyboardEventSource: source, virtualKey: 49, keyDown: false) else
		{
			throw TestFailure(description: "Cannot create native Quartz Space events")
		}
		down.flags = []
		up.flags = []
		// Both real events target this PID; no global event tap or internal engine
		// injection is used. InputState retains a press/release in one GLFW poll.
		down.postToPid(pid)
		up.postToPid(pid)
		try CheckFocused()
	}

	func Capture(region: CGRect? = nil) async throws -> Frame
	{
		try CheckPermissions(Permissions())
		try CheckFocused()
		guard let window else { throw TestFailure(description: "No window to capture") }
		let filter = SCContentFilter(desktopIndependentWindow: window)
		let configuration = SCStreamConfiguration()
		configuration.width = Int(window.frame.width.rounded())
		configuration.height = Int(window.frame.height.rounded())
		configuration.showsCursor = false
		configuration.ignoreShadowsSingleWindow = true
		let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: configuration)
		try CheckFocused()
		// Capture only the game interior. Window title bars, focus decoration and
		// the pointer cannot satisfy the rendered-game pixel-change assertion.
		try Require(image.width > 96 && image.height > 128 && image.width * image.height <= 16 * 1024 * 1024,
			"Invalid captured window dimensions")
		let interior = region ?? CGRect(x: 24, y: 64, width: CGFloat(image.width - 48), height: CGFloat(image.height - 88))
		try Require(interior.minX >= 0 && interior.minY >= 0 && interior.maxX <= CGFloat(image.width) &&
			interior.maxY <= CGFloat(image.height), "Capture region lies outside the owned window")
		guard let cropped = image.cropping(to: interior),
			let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
			let context = CGContext(data: nil, width: cropped.width, height: cropped.height, bitsPerComponent: 8,
				bytesPerRow: cropped.width * 4, space: colorSpace,
				bitmapInfo: CGBitmapInfo.byteOrder32Big.rawValue | CGImageAlphaInfo.premultipliedLast.rawValue) else
		{
			throw TestFailure(description: "Cannot normalize the captured game pixels")
		}
		context.draw(cropped, in: CGRect(x: 0, y: 0, width: CGFloat(cropped.width), height: CGFloat(cropped.height)))
		guard let raw = context.data?.assumingMemoryBound(to: UInt8.self) else
		{
			throw TestFailure(description: "Captured image has no pixels")
		}
		let count = cropped.width * cropped.height
		var pixels = Data(count: count * 3)
		pixels.withUnsafeMutableBytes { (bytes: UnsafeMutableRawBufferPointer) in
			for index in 0..<count
			{
				for channel in 0..<3 { bytes[index * 3 + channel] = raw[index * 4 + channel] }
			}
		}
		return Frame(width: cropped.width, height: cropped.height, pixels: pixels)
	}

	func WaitForImage(previous: String? = nil) async throws -> Frame
	{
		let deadline = ProcessInfo.processInfo.systemUptime + 30
		while ProcessInfo.processInfo.systemUptime < deadline
		{
			let frame = try await Capture()
			if frame.populated && frame.digest != previous { return frame }
			try await Task.sleep(nanoseconds: 20_000_000)
		}
		throw TestFailure(description: "Game did not present populated changed pixels")
	}

	func RequestClose() throws
	{
		try CheckFocused()
		guard let accessibilityWindow else { throw TestFailure(description: "No game window to close") }
		let button = try ElementAttribute(accessibilityWindow, kAXCloseButtonAttribute)
		try Require(try Owner(button) == pid, "Refusing another process's close button")
		try RequireAX(AXUIElementPerformAction(button, kAXPressAction as CFString), "Press owned window close button")
	}
}
