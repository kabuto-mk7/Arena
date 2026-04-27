import argparse
import pathlib
import sys
import bpy


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def convert_one(src_fbx: pathlib.Path, dst_glb: pathlib.Path):
    reset_scene()
    bpy.ops.import_scene.fbx(filepath=str(src_fbx), use_custom_normals=True)

    # Join all mesh parts so runtimes that only consume a single mesh still get the full character.
    mesh_objs = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    if mesh_objs:
        bpy.ops.object.select_all(action='DESELECT')
        for obj in mesh_objs:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = mesh_objs[0]
        bpy.ops.object.join()

    # Export with baked animation and skin for runtime compatibility.
    bpy.ops.export_scene.gltf(
        filepath=str(dst_glb),
        export_format='GLB',
        export_animations=True,
        export_bake_animation=True,
        export_skins=True,
        export_morph=False,
        export_apply=False,
        export_yup=True,
        export_texcoords=True,
        export_normals=True,
        export_tangents=False,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", required=True)
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    args = parser.parse_args(argv)

    assets_dir = pathlib.Path(args.assets_dir).resolve()
    fbx_files = sorted(assets_dir.glob("*.fbx"))
    if not fbx_files:
        print(f"No FBX files found in {assets_dir}")
        return

    for fbx in fbx_files:
        out_glb = fbx.with_suffix(".glb")
        print(f"Converting: {fbx.name} -> {out_glb.name}")
        convert_one(fbx, out_glb)

    print(f"Done. Converted {len(fbx_files)} file(s).")


if __name__ == "__main__":
    main()
