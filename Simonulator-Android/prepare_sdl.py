"""Prepare the SDL3 Android archive and arm64 linker inputs for the MAME build."""
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
from urllib.request import urlopen
from zipfile import ZipFile


VERSION = "3.4.20"
SHA256 = "535320e58f3bd7235b3e15f940595cdeeb9b8c0aa6b3810ff127345e3567d4da"


def prepare(archive, output):
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/{archive.name}"
        with urlopen(url, timeout=120) as response:
            archive.write_bytes(response.read())
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError(f"Unexpected SDL archive digest: {archive}")
    output.mkdir(parents=True, exist_ok=True)
    with ZipFile(BytesIO(data)) as bundle:
        aar = bundle.read(f"SDL3-{VERSION}.aar")
    (output / "SDL3.aar").write_bytes(aar)
    with ZipFile(BytesIO(aar)) as package:
        headers = "prefab/modules/SDL3-Headers/include/"
        for name in package.namelist():
            if name.startswith(headers) and not name.endswith("/"):
                file = output / "include" / name[len(headers):]
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(package.read(name))
        lib = output / "lib"
        lib.mkdir(exist_ok=True)
        (lib / "libSDL3.so").write_bytes(package.read(
            "prefab/modules/SDL3-shared/libs/android.arm64-v8a/libSDL3.so"))
    pc = lib / "pkgconfig"
    pc.mkdir(exist_ok=True)
    (pc / "sdl3.pc").write_text(
        f"prefix={output.resolve().as_posix()}\nName: sdl3\nDescription: SDL for Android arm64\n"
        f"Version: {VERSION}\nCflags: -I${{prefix}}/include\nLibs: -L${{prefix}}/lib -lSDL3\n",
        encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, default=Path(__file__).resolve().parent.parent /
                        "work" / "downloads" / f"SDL3-devel-{VERSION}-android.zip")
    args = parser.parse_args()
    prepare(args.archive, Path(__file__).resolve().parent / "build" / "sdl")
