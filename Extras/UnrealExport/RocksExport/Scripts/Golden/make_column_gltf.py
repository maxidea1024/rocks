# Writes the small skinned glTF 2.0 source of the golden bundle (RocksExport, Docs/Design/UnrealExchange section 12.3): a two-joint column with blended skin weights,
# one morph target ("Bulge") and an animation that bends the tip joint, moves the root and drives the morph weight.
import base64
import json
import struct
import sys

out = sys.argv[1]
rings = [0.0, 1.0, 2.0]
half = 0.1
corners = [(-half, -half), (half, -half), (half, half), (-half, half)]
positions = []
normals = []
uvs = []
joints = []
weights = []
for r, y in enumerate(rings):
    for c, (x, z) in enumerate(corners):
        positions += [x, y, z]
        length = (x * x + z * z) ** 0.5
        normals += [x / length, 0.0, z / length]
        uvs += [c / 3.0, r / 2.0]
        tip = r / 2.0
        joints += [0, 1, 0, 0]
        weights += [1.0 - tip, tip, 0.0, 0.0]

indices = []
for r in range(2):
    for c in range(4):
        a = r * 4 + c
        b = r * 4 + (c + 1) % 4
        indices += [a, a + 4, b, b, a + 4, b + 4]

bulge = []
for r in range(3):
    for (x, z) in corners:
        scale = 0.5 if r == 1 else 0.0
        bulge += [x * scale, 0.0, z * scale]

# Inverse bind matrices (column major): identity for the root, the inverse of the y = 1 translation for the tip.
inverse_bind = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] + [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]
times = [0.0, 0.5, 1.0]
tip_rotations = [0, 0, 0, 1, 0, 0, 0.3826834, 0.9238795, 0, 0, 0.7071068, 0.7071068]
root_translations = [0, 0, 0, 0.2, 0, 0, 0.4, 0, 0]
morph_weights = [0.0, 1.0, 0.0]

chunks = []
views = []
accessors = []


def add(data, fmt, count, kind, component, target=None, minmax=None):
    raw = struct.pack("<%d%s" % (len(data), fmt), *data)
    while len(b"".join(chunks)) % 4:
        chunks.append(b"\x00")
    offset = len(b"".join(chunks))
    chunks.append(raw)
    view = {"buffer": 0, "byteOffset": offset, "byteLength": len(raw)}
    if target is not None:
        view["target"] = target
    views.append(view)
    accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
    if minmax is not None:
        accessor["min"], accessor["max"] = minmax
    accessors.append(accessor)
    return len(accessors) - 1


def bounds(values):
    return [min(values[i::3]) for i in range(3)], [max(values[i::3]) for i in range(3)]


FLOAT = 5126
vertex_count = len(positions) // 3
a_pos = add(positions, "f", vertex_count, "VEC3", FLOAT, 34962, bounds(positions))
a_nrm = add(normals, "f", vertex_count, "VEC3", FLOAT, 34962)
a_uv = add(uvs, "f", vertex_count, "VEC2", FLOAT, 34962)
a_joint = add(joints, "B", vertex_count, "VEC4", 5121, 34962)
a_weight = add(weights, "f", vertex_count, "VEC4", FLOAT, 34962)
a_index = add(indices, "H", len(indices), "SCALAR", 5123, 34963)
a_bulge = add(bulge, "f", vertex_count, "VEC3", FLOAT, None, bounds(bulge))
a_ibm = add(inverse_bind, "f", 2, "MAT4", FLOAT)
a_time = add(times, "f", 3, "SCALAR", FLOAT, None, ([0.0], [1.0]))
a_rot = add(tip_rotations, "f", 3, "VEC4", FLOAT)
a_tr = add(root_translations, "f", 3, "VEC3", FLOAT)
a_mw = add(morph_weights, "f", 3, "SCALAR", FLOAT)

buffer = b"".join(chunks)
gltf = {
    "asset": {"version": "2.0", "generator": "RocksExport golden"},
    "scene": 0,
    "scenes": [{"nodes": [0, 2]}],
    "nodes": [
        {"name": "Root", "children": [1]},
        {"name": "Tip", "translation": [0, 1, 0]},
        {"name": "SK_Column", "mesh": 0, "skin": 0},
    ],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": a_ibm, "skeleton": 0}],
    "meshes": [{
        "name": "SK_Column",
        "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv, "JOINTS_0": a_joint, "WEIGHTS_0": a_weight},
                        "indices": a_index, "targets": [{"POSITION": a_bulge}]}],
        "weights": [0.0],
        "extras": {"targetNames": ["Bulge"]},
    }],
    "animations": [{
        "name": "Bend",
        "samplers": [{"input": a_time, "output": a_rot, "interpolation": "LINEAR"},
                     {"input": a_time, "output": a_tr, "interpolation": "LINEAR"},
                     {"input": a_time, "output": a_mw, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}},
                     {"sampler": 1, "target": {"node": 0, "path": "translation"}},
                     {"sampler": 2, "target": {"node": 2, "path": "weights"}}],
    }],
    "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(buffer).decode("ascii")}],
    "bufferViews": views,
    "accessors": accessors,
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(gltf, f, indent=1)
