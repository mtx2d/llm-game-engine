"""Native X11 readback and presentation synchronization for integration tests."""

import ctypes
import hashlib
import time


class WindowFrames:
    """Read presented X11 pixels so keyboard timing does not assume a GPU speed."""

    class Image(ctypes.Structure):
        # Only the prefix used here is needed; Xlib owns and destroys the full XImage.
        _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int),
                    ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                    ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                    ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                    ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                    ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int),
                    ("red_mask", ctypes.c_ulong), ("green_mask", ctypes.c_ulong),
                    ("blue_mask", ctypes.c_ulong)]

    def __init__(self, window, width, height):
        self.window, self.width, self.height = int(window), width, height
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                                      ctypes.c_ulong, ctypes.c_int]
        self.x11.XGetImage.restype = ctypes.POINTER(self.Image)
        self.x11.XDestroyImage.argtypes = [ctypes.POINTER(self.Image)]
        self.x11.XGetPixel.argtypes = [ctypes.POINTER(self.Image), ctypes.c_int, ctypes.c_int]
        self.x11.XGetPixel.restype = ctypes.c_ulong
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("Cannot connect to the native runtime test display")

    def close(self):
        if self.display:
            self.x11.XCloseDisplay(self.display)
            self.display = None

    def capture(self):
        image = self.x11.XGetImage(self.display, self.window, 0, 0,
                                  self.width, self.height, ctypes.c_ulong(-1), 2)
        if not image:
            raise RuntimeError("Cannot read the native runtime window")
        try:
            contents = image.contents
            count = contents.bytes_per_line * contents.height
            if not contents.data or not 0 < count <= 64 * 1024 * 1024:
                raise RuntimeError("Native window returned invalid image dimensions")
            pixels = ctypes.string_at(contents.data, count)
            return hashlib.sha256(pixels).digest(), len(set(pixels)) > 8
        finally:
            self.x11.XDestroyImage(image)

    def sample_rgb(self, points):
        """Sample presented drawable pixels using the native image's color masks."""
        image = self.x11.XGetImage(self.display, self.window, 0, 0,
                                  self.width, self.height, ctypes.c_ulong(-1), 2)
        if not image:
            raise RuntimeError("Cannot read the native editor window")
        try:
            masks = (image.contents.red_mask, image.contents.green_mask, image.contents.blue_mask)
            assert all(masks), "Native test display must expose RGB channels"
            shifts = [(mask & -mask).bit_length() - 1 for mask in masks]
            samples = []
            for x, y in points:
                assert 0 <= x < self.width and 0 <= y < self.height
                value = self.x11.XGetPixel(image, x, y)
                samples.append(tuple(((value & mask) >> shift) * 255 // (mask >> shift)
                                     for mask, shift in zip(masks, shifts)))
            return samples
        finally:
            self.x11.XDestroyImage(image)


class WindowRepaints:
    """Wait for actual drawable refreshes, independent of cursor movement and GPU speed.

    Xvfb Vulkan WSI copies each frame to the window in one or more rectangles,
    so Present-extension notifications are not available. Raw XDamage reports
    those copies even for identical images. The rectangle covering the final
    pixel marks a redraw tail; cursor sprites do not modify this drawable.
    """

    class Rectangle(ctypes.Structure):
        _fields_ = [("x", ctypes.c_short), ("y", ctypes.c_short),
                    ("width", ctypes.c_ushort), ("height", ctypes.c_ushort)]

    class Event(ctypes.Union):
        _fields_ = [("padding", ctypes.c_long * 24)]

    def __init__(self, window, width, height):
        class DamageEvent(ctypes.Structure):
            _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                        ("display", ctypes.c_void_p), ("drawable", ctypes.c_ulong), ("damage", ctypes.c_ulong),
                        ("level", ctypes.c_int), ("more", ctypes.c_int), ("timestamp", ctypes.c_ulong),
                        ("area", self.Rectangle), ("geometry", self.Rectangle)]

        self.damage_event_type = DamageEvent
        self.window, self.width, self.height = int(window), width, height
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.damage = ctypes.CDLL("libXdamage.so.1")
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XPending.argtypes = [ctypes.c_void_p]
        self.x11.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(self.Event)]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.damage.XDamageQueryExtension.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                                     ctypes.POINTER(ctypes.c_int)]
        self.damage.XDamageCreate.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int]
        self.damage.XDamageCreate.restype = ctypes.c_ulong
        self.display = self.x11.XOpenDisplay(None)
        try:
            if not self.display:
                raise RuntimeError("Cannot connect to the native repaint test display")
            event_base, error_base = ctypes.c_int(), ctypes.c_int()
            if not self.damage.XDamageQueryExtension(self.display, ctypes.byref(event_base), ctypes.byref(error_base)):
                raise RuntimeError("Native repaint synchronization requires the XDamage extension")
            self.event_base = event_base.value
            self.damage_id = self.damage.XDamageCreate(self.display, self.window, 0)
            self.x11.XFlush(self.display)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.display:
            self.x11.XCloseDisplay(self.display)
            self.display = None

    def drain(self):
        redraws = 0
        while self.x11.XPending(self.display):
            event = self.Event()
            self.x11.XNextEvent(self.display, ctypes.byref(event))
            damage = ctypes.cast(ctypes.byref(event), ctypes.POINTER(self.damage_event_type)).contents
            area = damage.area
            if damage.type == self.event_base and damage.drawable == self.window and \
                    damage.damage == self.damage_id and area.x <= self.width - 1 < area.x + area.width and \
                    area.y <= self.height - 1 < area.y + area.height:
                redraws += 1
        return redraws

    def wait(self, process, count=3, timeout=15):
        deadline = time.monotonic() + timeout
        observed = 0
        while time.monotonic() < deadline:
            assert process.poll() is None, "Native process exited while waiting for rendered frames"
            observed += self.drain()
            if observed >= count:
                return
            time.sleep(0.01)
        raise AssertionError(f"Native window completed {observed}/{count} required redraws")
