from __future__ import annotations

import os
from pathlib import Path

Import("env")


def candidate_pairs(project_dir: Path) -> list[tuple[Path, Path]]:
    pairs: list[tuple[Path, Path]] = []

    env_lib = os.environ.get("LXMF_RUST_FFI_LIB")
    env_include = os.environ.get("LXMF_RUST_FFI_INCLUDE")
    if env_lib and env_include:
        pairs.append((Path(env_lib), Path(env_include)))

    sibling_repo = project_dir.parent / "LXMF-rs"
    ffi_include = sibling_repo / "crates" / "libs" / "rns-embedded-ffi" / "include"
    target_globs = [
        sibling_repo / "target" / "xtensa-esp32-espidf" / "release" / "librns_embedded_ffi.a",
        sibling_repo / "target" / "xtensa-esp32-none-elf" / "release" / "librns_embedded_ffi.a",
        sibling_repo / "target" / "xtensa-esp32s3-none-elf" / "release" / "librns_embedded_ffi.a",
        sibling_repo / "target" / "release" / "librns_embedded_ffi.a",
    ]
    for lib_path in target_globs:
        pairs.append((lib_path, ffi_include))

    return pairs


def configure() -> None:
    project_dir = Path(env["PROJECT_DIR"]).resolve()

    for lib_path, include_dir in candidate_pairs(project_dir):
        if not lib_path.is_file():
            continue
        header_path = include_dir / "rns_embedded_ffi.h"
        if not header_path.is_file():
            continue

        env.Append(CPPPATH=[str(include_dir)])
        env.Append(LIBPATH=[str(lib_path.parent)])
        env.Append(LIBS=["rns_embedded_ffi"])
        env.Append(CPPDEFINES=[("LXMF_HAS_RUST_FFI_BUILD", 1)])
        print(
            "[lxmf-ffi] enabled rust ffi "
            f"lib={lib_path} include={include_dir}"
        )
        return

    print(
        "[lxmf-ffi] rust ffi not linked; using firmware stub bridge "
        "(set LXMF_RUST_FFI_LIB and LXMF_RUST_FFI_INCLUDE once the ESP Rust staticlib exists)"
    )


configure()
