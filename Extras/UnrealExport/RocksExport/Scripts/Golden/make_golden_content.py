# Creates the source assets of the golden exchange bundle under /Game/RxGolden (RocksExport,
# Docs/Design/UnrealExchange section 12.3). Run it twice in a UE 5.8 project that has the RocksExport plugin,
# each time in a new editor process:
#   UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="make_golden_content.py content <path to SK_Column.gltf>"
#   UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="make_golden_content.py redirect"
# The Substrate golden bundle (section 8.8) has its own step, run once in a project with r.Substrate=True:
#   UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="make_golden_content.py substrate"
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
#
# Substrate step (/Game/RxGoldenSubstrate):
# - M_GoldenSubstrate: two Slab BSDFs stacked with Vertical Layering into FrontMaterial; BaseColor is also connected
#   (Substrate mode does not read it).
# - M_GoldenSubstrateAttributes: material attributes mode with a Make Material Attributes node and a Slab BSDF in
#   FrontMaterial.
# - M_Golden: the same graph as the main step (FrontMaterial empty, so the legacy pin mode).
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


def make_material(root=ROOT, with_instance=True):
    material = tools.create_asset("M_Golden", root, unreal.Material, unreal.MaterialFactoryNew())
    color = materials.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    color.set_editor_property("constant", unreal.LinearColor(0.5, 0.4, 0.3, 1.0))
    materials.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = materials.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -300, 200)
    roughness.set_editor_property("parameter_name", "Roughness")
    roughness.set_editor_property("default_value", 0.6)
    materials.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    materials.recompile_material(material)
    save(material.get_path_name())
    if not with_instance:
        log("material %s" % material.get_path_name())
        return

    instance = tools.create_asset("MI_Golden", root, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    materials.set_material_instance_parent(instance, material)
    materials.set_material_instance_scalar_parameter_value(instance, "Roughness", 0.25)
    save(instance.get_path_name())
    log("material %s, instance %s" % (material.get_path_name(), instance.get_path_name()))


SUBSTRATE_ROOT = "/Game/RxGoldenSubstrate"


def make_slab(material, x, y, albedo):
    slab = materials.create_material_expression(material, unreal.MaterialExpressionSubstrateSlabBSDF, x, y)
    color = materials.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, x - 300, y)
    color.set_editor_property("constant", albedo)
    if not materials.connect_material_expressions(color, "", slab, "Diffuse Albedo"):
        raise RuntimeError("cannot connect the slab diffuse albedo")

    return slab


def make_substrate_materials():
    material = tools.create_asset("M_GoldenSubstrate", SUBSTRATE_ROOT, unreal.Material, unreal.MaterialFactoryNew())
    top = make_slab(material, -400, -200, unreal.LinearColor(0.8, 0.2, 0.1, 1.0))
    bottom = make_slab(material, -400, 200, unreal.LinearColor(0.1, 0.2, 0.8, 1.0))
    layer = materials.create_material_expression(material, unreal.MaterialExpressionSubstrateVerticalLayering, -100, 0)
    if not (materials.connect_material_expressions(top, "", layer, "Top")
            and materials.connect_material_expressions(bottom, "", layer, "Bottom")):
        raise RuntimeError("cannot connect the vertical layering inputs")

    if not materials.connect_material_property(layer, "", unreal.MaterialProperty.MP_FRONT_MATERIAL):
        raise RuntimeError("cannot connect FrontMaterial")

    color = materials.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 500)
    color.set_editor_property("constant", unreal.LinearColor(0.3, 0.3, 0.3, 1.0))
    materials.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    materials.recompile_material(material)
    save(material.get_path_name())

    attributes = tools.create_asset("M_GoldenSubstrateAttributes", SUBSTRATE_ROOT, unreal.Material, unreal.MaterialFactoryNew())
    attributes.set_editor_property("use_material_attributes", True)
    make = materials.create_material_expression(attributes, unreal.MaterialExpressionMakeMaterialAttributes, -300, 300)
    materials.connect_material_property(make, "", unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)
    slab = make_slab(attributes, -300, -100, unreal.LinearColor(0.2, 0.7, 0.2, 1.0))
    if not materials.connect_material_property(slab, "", unreal.MaterialProperty.MP_FRONT_MATERIAL):
        raise RuntimeError("cannot connect FrontMaterial of the attributes material")

    materials.recompile_material(attributes)
    save(attributes.get_path_name())
    log("substrate %s, attributes %s" % (material.get_path_name(), attributes.get_path_name()))
    make_material(SUBSTRATE_ROOT, False)


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
elif step == "substrate":
    if not unreal.SystemLibrary.get_console_variable_bool_value("r.Substrate"):
        raise RuntimeError("the substrate step needs a project with r.Substrate=True")

    if library.does_directory_exist(SUBSTRATE_ROOT):
        library.delete_directory(SUBSTRATE_ROOT)

    make_substrate_materials()
else:
    raise RuntimeError("usage: make_golden_content.py content <path to SK_Column.gltf> | redirect | substrate")

log("done %s" % step)
