"""Generate deterministic original glTF primitives used by the feature gallery."""
import base64
import json
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1] / "Assets" / "Models"
ROOT.mkdir(parents=True, exist_ok=True)


def checker_png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = bytearray()
    for y in range(16):
        pixels.append(0)
        for x in range(16):
            shade = 220 if (x // 4 + y // 4) % 2 else 130
            pixels.extend((shade, shade, shade, 255))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))


def write(name, vertices, indices, color, metallic, roughness, textured=False):
    vertex_data = b"".join(struct.pack("<8f", *vertex) for vertex in vertices)
    binary = vertex_data + struct.pack("<" + "I" * len(indices), *indices)
    material = {"name": name, "pbrMetallicRoughness": {
        "baseColorFactor": color, "metallicFactor": metallic, "roughnessFactor": roughness}}
    document = {
        "asset": {"version": "2.0", "generator": "Aster original procedural test fixtures"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(binary), "uri": "data:application/octet-stream;base64," + base64.b64encode(binary).decode()}],
        "bufferViews": [{"buffer": 0, "byteLength": len(vertex_data), "byteStride": 32},
                        {"buffer": 0, "byteOffset": len(vertex_data), "byteLength": len(binary) - len(vertex_data)}],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": len(vertices), "type": "VEC3",
             "min": [min(vertex[i] for vertex in vertices) for i in range(3)],
             "max": [max(vertex[i] for vertex in vertices) for i in range(3)]},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": len(vertices), "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": len(vertices), "type": "VEC2"},
            {"bufferView": 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0}]}], "materials": [material]}
    if textured:
        document["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(checker_png()).decode()}]
        document["textures"] = [{"source": 0}]
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    (ROOT / (name + ".gltf")).write_text(json.dumps(document, separators=(",", ":")) + "\n")


vertices, indices = [], []
faces = [
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)])]
for normal, corners in faces:
    start = len(vertices)
    for corner, uv in zip(corners, [(0, 0), (1, 0), (1, 1), (0, 1)]):
        vertices.append(tuple(c * 0.5 for c in corner) + normal + uv)
    indices += [start, start + 1, start + 2, start, start + 2, start + 3]
write("Cube", vertices, indices, [0.5, 0.65, 0.85, 1], 0.25, 0.4)
write("Floor", vertices, indices, [0.7, 0.75, 0.8, 1], 0, 0.85, True)

vertices, indices = [], []
for row in range(17):
    theta = math.pi * row / 16
    for column in range(25):
        phi = 2 * math.pi * column / 24
        normal = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
        vertices.append(tuple(c * 0.5 for c in normal) + normal + (column / 24, row / 16))
for row in range(16):
    for column in range(24):
        a, b = row * 25 + column, (row + 1) * 25 + column
        if row != 0:
            indices += [a, a + 1, b]
        if row != 15:
            indices += [a + 1, b + 1, b]
write("Sphere", vertices, indices, [1, 0.64, 0.2, 1], 0.85, 0.22)

vertices, indices = [], []
for ring in range(33):
    phi = 2 * math.pi * ring / 32
    for side in range(13):
        theta = 2 * math.pi * side / 12
        normal = (math.cos(theta) * math.cos(phi), math.sin(theta), math.cos(theta) * math.sin(phi))
        position = ((0.65 + 0.2 * math.cos(theta)) * math.cos(phi), 0.2 * math.sin(theta),
                    (0.65 + 0.2 * math.cos(theta)) * math.sin(phi))
        vertices.append(position + normal + (ring / 32, side / 12))
for ring in range(32):
    for side in range(12):
        a, b = ring * 13 + side, (ring + 1) * 13 + side
        indices += [a, a + 1, b, a + 1, b + 1, b]
write("Torus", vertices, indices, [0.3, 0.75, 0.9, 1], 0.6, 0.25)
print("Generated Cube, Floor, Sphere, and Torus glTF fixtures")
