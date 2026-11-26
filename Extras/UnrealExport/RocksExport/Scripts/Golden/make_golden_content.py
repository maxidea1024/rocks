# Creates the source assets of the golden exchange bundle under /Game/RxGolden (RocksExport,
# Docs/Design/UnrealExchange section 12.3). Run it twice in a UE 5.8 project that has the RocksExport plugin,
# each time in a new editor process:
#   UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="make_golden_content.py content <path to SK_Column.gltf>"
#   UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="make_golden_content.py redirect"
# The glTF comes from make_column_gltf.py. The content step deletes /Game/RxGolden first, so the result does not depend
# on earlier runs (expression and import GUIDs are new each time; the golden test does not compare them).
#
# Assets:
# - SK_Column (+ skeleton, physics asset, clip): two-joint column with blended weights, morph target "Bulge" and a
#   three-key clip, imported through Interchange.
# - SM_Degenerate: one quad whose perimeter repeats a vertex (UE makes a self edge for it) and one triangle (OBJ).
# - SK_SoftTarget: copy of the engine SkeletalCube. The clip references it softly by the old path SK_SoftTarget_Old:
#   the content step saves the clip pointing at a second copy SK_SoftTarget_Old, and the redirect step (a new process,
#   so the clip is not loaded and keeps its saved path) consolidates that copy into SK_SoftTarget, which leaves a
#   redirector at the old path.
# - M_Golden / MI_Golden: small material (constant base color, scalar parameter) and an instance overriding it.
import sys

import unreal

ROOT = "/Game/RxGolden"
tools = unreal.AssetToolsHelpers.get_asset_tools()
library = unreal.EditorAssetLibrary
materials = unreal.MaterialEditingLibrary


def log(text):
    unreal.log("RXGOLDEN %s" % text)


def save(path):
    if not library.save_asset(path, only_if_is_dirty=False):
        raise RuntimeError("cannot save %s" % path)


def import_column(gltf):
    task = unreal.AssetImportTask()
    task.filename = gltf
    task.destination_path = ROOT
    task.automated = True
    task.save = True
    task.replace_existing = True
    tools.import_asset_tasks([task])
    paths = list(task.imported_object_paths)
    for path in paths:
        log("imported %s" % path)
    return paths


def make_degenerate_mesh():
    # Python cannot call MeshDescription CreatePolygon (its corner array is an out parameter there), so the mesh comes
    # from an OBJ file: the face 1-2-2-3 repeats vertex 2, so UE creates the edge (2, 2), and 1-3-4 is a triangle.
    folder = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_intermediate_dir()) + "RxGolden"
    unreal.Paths.normalize_directory_name(folder)
    obj = folder + "/SM_Degenerate.obj"
    import os

    os.makedirs(folder, exist_ok=True)
    with open(obj, "w", encoding="ascii", newline="\n") as file:
        file.write("o SM_Degenerate\n")
        for x, y in [(0, 0), (100, 0), (100, 100), (0, 100)]:
            file.write("v %d %d 0\n" % (x, y))
        for u, v in [(0, 0), (1, 0), (1, 1), (0, 1)]:
            file.write("vt %d %d\n" % (u, v))
        file.write("vn 0 0 1\n")
        file.write("f 1/1/1 2/2/1 2/2/1 3/3/1\n")
        file.write("f 1/1/1 3/3/1 4/4/1\n")

    task = unreal.AssetImportTask()
    task.filename = obj
    task.destination_path = ROOT
    task.automated = True
    task.save = True
    task.replace_existing = True
    tools.import_asset_tasks([task])
    for path in task.imported_object_paths:
        log("degenerate mesh %s" % path)


def make_material():
    material = tools.create_asset("M_Golden", ROOT, unreal.Material, unreal.MaterialFactoryNew())
    color = materials.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    color.set_editor_property("constant", unreal.LinearColor(0.5, 0.4, 0.3, 1.0))
    materials.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = materials.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -300, 200)
    roughness.set_editor_property("parameter_name", "Roughness")
    roughness.set_editor_property("default_value", 0.6)
    materials.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    materials.recompile_material(material)
    save(material.get_path_name())
    instance = tools.create_asset("MI_Golden", ROOT, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    materials.set_material_instance_parent(instance, material)
    materials.set_material_instance_scalar_parameter_value(instance, "Roughness", 0.25)
    save(instance.get_path_name())
    log("material %s, instance %s" % (material.get_path_name(), instance.get_path_name()))


SOFT_OLD = ROOT + "/SK_SoftTarget_Old"
SOFT_NEW = ROOT + "/SK_SoftTarget"


def make_soft_targets(clip_path):
    for path in (SOFT_OLD, SOFT_NEW):
        if library.duplicate_asset("/Engine/EngineMeshes/SkeletalCube", path) is None:
            raise RuntimeError("cannot copy SkeletalCube to %s" % path)
        save(path)

    clip = unreal.load_asset(clip_path)
    clip.set_editor_property("retarget_source_asset", unreal.load_asset(SOFT_OLD))
    save(clip_path)
    log("clip %s points at %s" % (clip_path, SOFT_OLD))


def redirect_soft_target():
    if not library.consolidate_assets(unreal.load_asset(SOFT_NEW), [unreal.load_asset(SOFT_OLD)]):
        raise RuntimeError("cannot consolidate %s into %s" % (SOFT_OLD, SOFT_NEW))
    log("redirector %s -> %s" % (SOFT_OLD, SOFT_NEW))


step = sys.argv[1] if len(sys.argv) > 1 else ""
# Commandlets do not scan the asset registry by themselves (engine content would be missing).
unreal.AssetRegistryHelpers.get_asset_registry().search_all_assets(True)
if step == "content" and len(sys.argv) > 2:
    if library.does_directory_exist(ROOT):
        library.delete_directory(ROOT)

    imported = import_column(sys.argv[2])
    clips = [path for path in imported if isinstance(unreal.load_asset(path), unreal.AnimSequence)]
    if len(clips) != 1:
        raise RuntimeError("expected one imported clip, got %s" % clips)

    make_degenerate_mesh()
    make_material()
    make_soft_targets(clips[0].split(".")[0])
elif step == "redirect":
    redirect_soft_target()
else:
    raise RuntimeError("usage: make_golden_content.py content <path to SK_Column.gltf> | redirect")

log("done %s" % step)
