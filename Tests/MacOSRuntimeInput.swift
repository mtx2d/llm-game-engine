import AppKit
import Darwin
import Foundation

@main
struct NativeInput
{
	@MainActor
	static func main() async
	{
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
			try Require(arguments.count == 3, "Expected launched PID, executable path and artifact directory")
			guard let pid = Int32(arguments[0]) else { throw TestFailure(description: "Invalid launched PID") }
			let artifacts = URL(fileURLWithPath: arguments[2], isDirectory: true)
			let game = try GameWindow(pid: pid, executable: URL(fileURLWithPath: arguments[1]))
			try await game.FindAndFocus()
			let before = try await game.WaitForImage()
			try game.Space()
			let after = try await game.WaitForImage(previous: before.digest)
			try game.RequestClose()
			try before.Save(artifacts.appendingPathComponent("MacOSRuntimeInputBefore.ppm"))
			try after.Save(artifacts.appendingPathComponent("MacOSRuntimeInputAfter.ppm"))
			let result: [String: Any] = ["pid": pid, "window_id": game.window!.windowID, "permissions": permissions,
				"before_sha256": before.digest, "after_sha256": after.digest, "close_requested": true,
				"focus_retries": game.focusRetries]
			try WriteJSON(result)
		}
		catch
		{
			let diagnostic = Data("Native macOS input: \(error)\n".utf8)
			do
			{
				try FileHandle.standardError.write(contentsOf: diagnostic)
			}
			catch
			{
				exit(2)
			}
			exit(1)
		}
	}
}
