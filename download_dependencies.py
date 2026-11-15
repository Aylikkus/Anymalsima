#!/usr/bin/env python3
"""Download the project's pinned source dependencies (Python 3.12+)."""

import argparse
import http.client
from pathlib import Path, PurePosixPath
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request


def github_archive(repository, revision):
    return f"https://github.com/{repository}/archive/{revision}.tar.gz"


# Destination, source archive, and files required by our CMake setup.
# The bgfx release asset includes bx, bimg, and bgfx; GitHub's automatic
# source archive does not include those submodules.
DEPENDENCIES = (
    (
        "bgfx.cmake-1.143-9241-532",
        "https://github.com/bkaradzic/bgfx.cmake/releases/download/"
        "v1.143.9241-532/bgfx.cmake.v1.143.9241-532.tar.gz",
        ("CMakeLists.txt", "bgfx/include/bgfx/bgfx.h",
         "bx/include/bx/bx.h", "bimg/include/bimg/bimg.h"),
    ),
    ("entt-3.16.0", github_archive("skypjack/entt", "v3.16.0"),
     ("CMakeLists.txt", "src/entt/entt.hpp")),
    ("fmt-12.1.0", github_archive("fmtlib/fmt", "12.1.0"),
     ("CMakeLists.txt", "include/fmt/format.h")),
    ("json-3.12.0", github_archive("nlohmann/json", "v3.12.0"),
     ("CMakeLists.txt", "include/nlohmann/json.hpp")),
    ("miniaudio-0.11.25", github_archive("mackron/miniaudio", "0.11.25"),
     ("CMakeLists.txt", "miniaudio.h")),
    ("SDL3-3.4.8", github_archive("libsdl-org/SDL", "release-3.4.8"),
     ("CMakeLists.txt", "include/SDL3/SDL.h")),
    ("thread-pool-5.1.0", github_archive("bshoshany/thread-pool", "v5.1.0"),
     ("include/BS_thread_pool.hpp",)),
    ("tracy-eb3df3b-0.13.1", github_archive(
        "wolfpld/tracy", "eb3df3b411438bbe0ae0e3d989427a14019d3270"),
     ("CMakeLists.txt", "public/tracy/Tracy.hpp")),
)


def validate(directory, required):
    missing = [name for name in required if not (directory / name).is_file()]
    if missing:
        raise ValueError(f"{directory}: missing {', '.join(missing)}")


def install_dependency(external, name, url, required):
    destination = external / name
    if destination.exists() or destination.is_symlink():
        if not destination.is_dir() or destination.is_symlink():
            raise ValueError(f"{destination}: expected a regular directory")
        validate(destination, required)
        print(f"Skipping {name}: already exists (version not verified).",
              flush=True)
        return

    print(f"Downloading {name}...", flush=True)
    with tempfile.TemporaryDirectory(prefix=".download-", dir=external) as tmp:
        temporary = Path(tmp)
        archive = temporary / "source.tar.gz"
        request = urllib.request.Request(
            url, headers={"User-Agent": "Anymalsima-dependency-setup"})
        with urllib.request.urlopen(request, timeout=60) as response:
            with archive.open("wb") as output:
                shutil.copyfileobj(response, output)
            expected = response.headers.get("Content-Length")
            if expected and archive.stat().st_size != int(expected):
                raise ValueError("Incomplete download")

        extracted = temporary / "source"
        extracted.mkdir()
        with tarfile.open(archive, "r:gz") as source:
            for member in source.getmembers():
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"Unsafe archive path: {member.name}")
            # data_filter rejects escaping paths/links and special files.
            source.extractall(extracted, filter="data")
        entries = list(extracted.iterdir())
        root = (entries[0] if len(entries) == 1 and entries[0].is_dir()
                else extracted)
        validate(root, required)
        if destination.exists() or destination.is_symlink():
            raise ValueError(f"{destination}: appeared during download")
        root.rename(destination)
    print(f"Installed {name}.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or newer is required")

    external = Path(__file__).resolve().parent / "external"
    name = "external directory"
    try:
        external.mkdir(exist_ok=True)
        for name, url, required in DEPENDENCIES:
            install_dependency(external, name, url, required)
    except (OSError, EOFError, ValueError, tarfile.TarError,
            http.client.HTTPException, urllib.error.URLError) as error:
        print(f"Error setting up {name}: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nDownload interrupted; rerun to continue.", file=sys.stderr)
        return 1
    print("Dependencies are ready. Using the repository's bundled stb package.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
