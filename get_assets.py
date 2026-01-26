import os
import shutil
from concurrent.futures import ThreadPoolExecutor, as_completed

# Source folders (from your pipeline run)
BASE_RUN = r"C:\Work\Projects\terrain_download_python\run_20260122_051401"
HEIGHT_SRC = os.path.join(BASE_RUN, "dds_height")
ALBEDO_SRC = os.path.join(BASE_RUN, "dds_albedo")

# Destination root = current working directory (run this from your engine project)
DEST_ROOT = os.getcwd()

MAX_WORKERS = 16


def copy_one_file(src_path, dest_path):
    """Copy a single file (used by threads)."""
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    shutil.copy2(src_path, dest_path)
    return os.path.basename(src_path)


def copy_dds_folder_multithreaded(src_folder, dest_subfolder):
    """
    Copy all .dds files from src_folder into DEST_ROOT/dest_subfolder
    using multithreading.
    """
    dest_folder = os.path.join(DEST_ROOT, dest_subfolder)
    os.makedirs(dest_folder, exist_ok=True)

    files = [
        f for f in os.listdir(src_folder)
        if f.lower().endswith(".dds")
    ]

    print(f"Found {len(files)} DDS files in {src_folder}")

    futures = []
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as exe:
        for filename in files:
            src_path = os.path.join(src_folder, filename)
            dest_path = os.path.join(dest_folder, filename)
            futures.append(exe.submit(copy_one_file, src_path, dest_path))

        for fut in as_completed(futures):
            copied = fut.result()
            print(f"Copied {copied}")


print("Copying height tiles...")
copy_dds_folder_multithreaded(HEIGHT_SRC, os.path.join("data", "height"))

print("\nCopying albedo tiles...")
copy_dds_folder_multithreaded(ALBEDO_SRC, os.path.join("data", "albedo"))

print("\nDone!")
