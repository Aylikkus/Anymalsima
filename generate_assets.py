#!/usr/bin/env python3
import os
import json
import subprocess

# You need to build the project once
# in order to have shaderc executable
SHADERC_PATH = "build/external/bgfx.cmake-1.143-9241-532/cmake/bgfx/shaderc"

ASSETS_DIR = "assets"
SHADERS_DIR = "shaders"
MATERIALS_DIR = "materials"
TEXTURES_DIR = "textures"
DEFINITIONS_FILE = os.path.join(ASSETS_DIR, "definitions.json")

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".webp", ".tga"}
AUDIO_EXT = {".flac", ".ogg", ".wav", ".mp3"}
JSON_EXT = {".json"}
VERT_SHADER_EXT = ".vert"
FRAG_SHADER_EXT = ".frag"
BIN_VERT_SHADER_EXT = ".vs.bin"
BIN_FRAG_SHADER_EXT = ".fs.bin"

def compile_shaders():
    for root, _, files in os.walk(SHADERS_DIR):
        for f in files:
            file = os.path.join(root, f)
            rel = os.path.relpath(file, SHADERS_DIR) 
            basename = os.path.splitext(os.path.basename(rel))[0]
            ext = os.path.splitext(file)[1].lower()
            if ext == VERT_SHADER_EXT:
                process = subprocess.run(
                    [
                        SHADERC_PATH,
                        "-f", file,
                        "-o", os.path.join(ASSETS_DIR, "shaders", basename+BIN_VERT_SHADER_EXT),
                        "--type", "v",
                        "-p", "spirv",
                        "--varyingdef", os.path.join(root, "varying.def.sc"),
                        "-i", os.path.join("external", "bgfx.cmake-1.143-9241-532", "bgfx", "src"),
                    ],
                    capture_output=True,
                )
                print(process.stdout.decode())
            if ext == FRAG_SHADER_EXT:
                process = subprocess.run(
                    [
                        SHADERC_PATH,
                        "-f", file,
                        "-o", os.path.join(ASSETS_DIR, "shaders", basename+BIN_FRAG_SHADER_EXT),
                        "--type", "f",
                        "-p", "spirv",
                        "--varyingdef", os.path.join(root, "varying.def.sc"),
                        "-i", os.path.join("external", "bgfx.cmake-1.143-9241-532", "bgfx", "src"),
                    ],
                    capture_output=True,
                )
                print(process.stdout.decode())


def load_definitions():
    if os.path.exists(DEFINITIONS_FILE):
        with open(DEFINITIONS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_definitions(data):
    with open(DEFINITIONS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def normalize(path):
    return path.replace("\\", "/")


def scan_assets():
    textures = {}
    music = {}
    sounds = {}
    shaders = {}
    materials = {}

    for root, dirs, files in os.walk(ASSETS_DIR):
        dirs[:] = [d for d in dirs if d not in ("i18n", "fonts", "items")]

        for file in files:
            ext = os.path.splitext(file)[1].lower()
            full_path = normalize(os.path.join(root, file))
            rel_path = normalize(os.path.relpath(full_path, ASSETS_DIR))

            if rel_path.startswith("i18n/") or rel_path.startswith("fonts/"):
                continue

            key = os.path.splitext(rel_path)[0].replace("/", ".")

            sh_ext = rel_path[rel_path.find("."):]

            if rel_path.startswith(TEXTURES_DIR+"/") and ext in IMAGE_EXT:
                textures[key.replace("textures.", "")] = f"assets/{rel_path}"

            elif rel_path.startswith(MATERIALS_DIR+"/") and ext in IMAGE_EXT:
                materials[key.replace("materials.", "")] = f"assets/{rel_path}"

            elif ext in AUDIO_EXT:
                if "/music/" in rel_path:
                    music[key.replace("audio.music.", "")] = f"assets/{rel_path}"
                elif "/sounds/" in rel_path:
                    sounds[key.replace("audio.sounds.", "")] = f"assets/{rel_path}"

            elif sh_ext == BIN_VERT_SHADER_EXT or sh_ext == BIN_FRAG_SHADER_EXT:
                sh_key = rel_path.replace(sh_ext, "").replace("/", ".")
                shaders[sh_key.replace("shaders.", "")] = f"assets/{rel_path.replace(sh_ext, "")}"

    return textures, music, sounds, shaders, materials


def scan_data_assets():
    data_root = os.path.join(ASSETS_DIR, "data")
    result = []

    if not os.path.exists(data_root):
        return result

    for data_type in os.listdir(data_root):
        type_path = os.path.join(data_root, data_type)

        if not os.path.isdir(type_path):
            continue

        files = []

        for root, _, filenames in os.walk(type_path):
            for file in filenames:
                ext = os.path.splitext(file)[1].lower()
                if ext not in JSON_EXT:
                    continue

                full_path = normalize(os.path.join(root, file))
                rel_path = normalize(os.path.relpath(full_path, ASSETS_DIR))

                files.append(f"assets/{rel_path}")

        if files:
            result.append({
                "type": data_type,
                "files": sorted(files)
            })

    return result


def main():
    compile_shaders()
    old_def = load_definitions()

    textures, music, sounds, shaders, materials = scan_assets()
    data = scan_data_assets()

    new_def = {}

    # Preserve fonts exactly as-is
    if "fonts" in old_def:
        new_def["fonts"] = old_def["fonts"]

    new_def["textures"] = dict(sorted(textures.items()))
    new_def["materials"] = dict(sorted(materials.items()))
    new_def["music"] = dict(sorted(music.items()))
    new_def["sounds"] = dict(sorted(sounds.items()))
    new_def["shaders"] = dict(sorted(shaders.items()))
    new_def["data"] = data

    save_definitions(new_def)


if __name__ == "__main__":
    main()