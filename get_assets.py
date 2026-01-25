import os
import shutil

HEIGHT_SRC = r"C:\Work\Projects\terrain_download_python\run_20260122_051401\dds_height"
ALBEDO_SRC = r"C:\Work\Projects\terrain_download_python\run_20260122_051401\dds_albedo"

DEST_ROOT = r"C:\Work\Projects\terrain"   # <-- TODO change this to automatically get current root
heightmapFilenames = [
    r"data\height\chunk_1079_720_1094_735_height.dds",
    r"data\height\chunk_1095_720_1110_735_height.dds",
    r"data\height\chunk_1111_720_1126_735_height.dds",

    r"data\height\chunk_1079_736_1094_751_height.dds",
    r"data\height\chunk_1095_736_1110_751_height.dds",
    r"data\height\chunk_1111_736_1126_751_height.dds",

    r"data\height\chunk_1079_752_1094_767_height.dds",
    r"data\height\chunk_1095_752_1110_767_height.dds",
    r"data\height\chunk_1111_752_1126_767_height.dds"
]

albedoFilenames = [
    r"data\albedo\chunk_1079_720_1094_735_albedo.dds",
    r"data\albedo\chunk_1095_720_1110_735_albedo.dds",
    r"data\albedo\chunk_1111_720_1126_735_albedo.dds",

    r"data\albedo\chunk_1079_736_1094_751_albedo.dds",
    r"data\albedo\chunk_1095_736_1110_751_albedo.dds",
    r"data\albedo\chunk_1111_736_1126_751_albedo.dds",

    r"data\albedo\chunk_1079_752_1094_767_albedo.dds",
    r"data\albedo\chunk_1095_752_1110_767_albedo.dds",
    r"data\albedo\chunk_1111_752_1126_767_albedo.dds"
]

def copy_files(file_list, src_folder):
    for path in file_list:
        dest_path = os.path.join(DEST_ROOT, path)
        os.makedirs(os.path.dirname(dest_path), exist_ok=True)

        filename = os.path.basename(path)
        src_path = os.path.join(src_folder, filename)

        print(f"Copying {filename}...")
        shutil.copy2(src_path, dest_path)

copy_files(heightmapFilenames, HEIGHT_SRC)
copy_files(albedoFilenames, ALBEDO_SRC)

print("Done!")
