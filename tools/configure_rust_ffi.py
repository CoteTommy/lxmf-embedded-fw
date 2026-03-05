from __future__ import annotations

import os
from pathlib import Path

Import("env")


def normalize_path(project_dir: Path, raw: str) -> Path:
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = (project_dir / path).resolve()
    return path.resolve()


def candidate_pairs(project_dir: Path) -> list[tuple[Path, Path]]:
    pairs: list[tuple[Path, Path]] = []

    env_lib = os.environ.get("LXMF_RUST_FFI_LIB")
    env_include = os.environ.get("LXMF_RUST_FFI_INCLUDE")
    if env_lib and env_include:
        pairs.append((normalize_path(project_dir, env_lib), normalize_path(project_dir, env_include)))

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

    node_defines: list[object] = []
    if os.environ.get("LXMF_NODE_MODE_TCP_CLIENT"):
        node_defines.append(("LXMF_NODE_MODE_TCP_CLIENT", 1))
    if os.environ.get("LXMF_NODE_MODE_TCP_SERVER"):
        node_defines.append(("LXMF_NODE_MODE_TCP_SERVER", 1))

    wifi_ssid = os.environ.get("LXMF_WIFI_SSID")
    wifi_password = os.environ.get("LXMF_WIFI_PASSWORD")
    tcp_host = os.environ.get("LXMF_TCP_HOST")
    tcp_port = os.environ.get("LXMF_TCP_PORT")
    capture_profile = os.environ.get("LXMF_CAPTURE_PROFILE", "").strip().lower()
    if wifi_ssid:
        node_defines.append(("LXMF_WIFI_SSID", env.StringifyMacro(wifi_ssid)))
    if wifi_password:
        node_defines.append(("LXMF_WIFI_PASSWORD", env.StringifyMacro(wifi_password)))
    if tcp_host:
        node_defines.append(("LXMF_TCP_HOST", env.StringifyMacro(tcp_host)))
    if tcp_port:
        node_defines.append(("LXMF_TCP_PORT", tcp_port))
    if capture_profile == "balanced":
        node_defines.append(("LXMF_CAPTURE_PROFILE_BALANCED", 1))
    elif capture_profile == "high":
        node_defines.append(("LXMF_CAPTURE_PROFILE_HIGH", 1))
    elif capture_profile == "thumbnail":
        node_defines.append(("LXMF_CAPTURE_PROFILE_THUMBNAIL", 1))
    if node_defines:
        env.Append(CPPDEFINES=node_defines)
        print(f"[lxmf-node] applied env build config defines={node_defines}")

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
