"""AT-4: our FBX and OBJ exports open in Blender with names, metres and Y-up intact.

Run with Blender:  blender --background --factory-startup --python tools/blender_check.py -- scene.fbx [scene.obj]
or with the bpy module:  python3 tools/blender_check.py scene.fbx [scene.obj]
Exits non-zero on the first failed check. Expects an export of the demo rig (trusses above a deck, a camera).
"""
import sys

import bpy
from mathutils import Vector


def world_bounds(objects):
    lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
    for o in objects:
        for corner in o.bound_box:
            p = o.matrix_world @ Vector(corner)
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
    return lo, hi


def check(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.lower().endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        bpy.ops.wm.obj_import(filepath=path, forward_axis="NEGATIVE_Z", up_axis="Y", use_split_groups=True)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    names = {o.name for o in bpy.data.objects}
    assert meshes, f"{path}: no meshes"
    assert any(n.endswith("_screen") for n in names), f"{path}: no wall screens in {sorted(names)[:10]}"
    lo, hi = world_bounds(meshes)
    size = hi - lo
    # Metres: the demo venue is tens of metres, not thousands (cm) or hundredths. Blender is Z up, so our Y-up
    # heights land on Z and the rig stands on the floor.
    assert 5 < max(size) < 200, f"{path}: scene size {tuple(size)} is not in metres"
    assert hi.z > 2 and lo.z > -1, f"{path}: Z range {lo.z:.2f}..{hi.z:.2f}, the rig is not upright"
    if path.lower().endswith(".fbx"):
        assert "Trusses" in names, f"{path}: no Trusses group"
        cams = [o for o in bpy.data.objects if o.type == "CAMERA"]
        assert len(cams) >= 2, f"{path}: expected the camera object and a bookmark, got {cams}"
        for cam in cams:  # looks roughly at the stage: forward (-Z local) points toward the stage, not the sky
            forward = cam.matrix_world.to_3x3() @ Vector((0, 0, -1))
            assert abs(forward.z) < 0.5 and forward.y > 0.5, f"{path}: camera {cam.name} looks along {tuple(forward)}"
    cams = [o.name for o in bpy.data.objects if o.type == "CAMERA"]
    print(f"ok {path}: {len(meshes)} meshes, cameras {cams}, size {size.x:.1f} x {size.y:.1f} x {size.z:.1f} m")


args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
if not args:
    sys.exit(__doc__)
for a in args:
    check(a)
