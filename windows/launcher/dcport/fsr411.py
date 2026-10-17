# SPDX-License-Identifier: GPL-2.0-or-later
"""FSR 4.1.1 set-up: the runtime's Vulkan replay of AMD's FSR 4.1.1 model needs an asset set
(fsr4_411/: the model's passes as SPIR-V and its weights). It is built here, on the player's PC,
from the player's own amd_fidelityfx_upscaler_dx12.dll; nothing of AMD's is in the release.

bin/fsr411/ (FSR edition) holds the tools: fsr4cap.exe runs the DLL once on D3D12 and records
what it does, dxil-spirv translates the shaders, the scripts check and assemble the set. AMD's
FidelityFX loader (and, for DLLs whose rewritten shaders lost their resource names, AMD's original
upscaler DLL, only read) are downloaded from AMD's public FidelityFX SDK repository, pinned and
checked. Both are under the SDK's MIT licence.
"""

import contextlib
import glob  # noqa: F401  (the scripts run below import these: bundled with the launcher)
import hashlib
import io
import math  # noqa: F401
import os
import re  # noqa: F401
import runpy
import shutil
import struct  # noqa: F401
import subprocess
import sys
import tempfile  # noqa: F401
import urllib.request
from collections.abc import Callable
from pathlib import Path

from dcport.paths import AppPaths

SDK = ("https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/"
       "60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX/signedbin/")
LOADER = ("amd_fidelityfx_loader_dx12.dll",
          "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa")
ORIGINAL = ("amd_fidelityfx_upscaler_dx12.dll",
            "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46")
# Native AA at 1080p and 1440p: the tiers DRIVECLUB uses (t1080, t2160).
SIZES = ("1920x1080", "2560x1440")
NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW


class SetupError(Exception):
    pass


def tools_dir(paths: AppPaths) -> Path:
    return paths.runtime.parent / "fsr411"


def assets_dir(paths: AppPaths) -> Path:
    return paths.root / "fsr4_411"


def available(paths: AppPaths) -> bool:
    """A complete set: both tiers with their weights."""
    a = assets_dir(paths)
    return all((a / t / "initializer.bin").is_file() and (a / t / "postpass.spv").is_file()
               for t in ("t1080_m0", "t2160_m0"))


def tools_present(paths: AppPaths) -> bool:
    t = tools_dir(paths)
    return all((t / f).is_file() for f in ("fsr4cap.exe", "dxil-spirv.exe", "spirv-as.exe",
                                            "spirv-dis.exe", "extract.py", "name_bindings.py",
                                            "postpass_lds.py"))


def _download(name: str, sha256: str, target: Path, report: Callable[[str], None]) -> None:
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == sha256:
        return
    report(f"Downloading {name} from AMD's FidelityFX SDK…")
    try:
        with urllib.request.urlopen(SDK + name, timeout=60) as r:
            data = r.read()
    except OSError as e:
        raise SetupError(f"Could not download {name} from GitHub: {e}") from None
    if hashlib.sha256(data).hexdigest() != sha256:
        raise SetupError(f"{name} from GitHub is not the expected file")
    target.write_bytes(data)


def _script(path: Path, argv: list[str]) -> tuple[int, str]:
    """One of the tool scripts in this process (the packaged launcher has no separate Python)."""
    out = io.StringIO()
    saved = sys.argv
    sys.argv = [str(path), *argv]
    code = 0
    real_run = subprocess.run

    def quiet_run(*args, **kwargs):  # the tools are console programs: no window each time
        kwargs.setdefault("creationflags", NO_WINDOW)
        return real_run(*args, **kwargs)

    subprocess.run = quiet_run
    try:
        with contextlib.redirect_stdout(out):
            runpy.run_path(str(path), run_name="__main__")
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
        if isinstance(e.code, str):
            out.write(e.code + "\n")
    except (OSError, subprocess.CalledProcessError) as e:
        code = 1
        out.write(f"{e}\n")
    finally:
        sys.argv = saved
        subprocess.run = real_run
    return code, out.getvalue()


def build(paths: AppPaths, dll: Path, report: Callable[[str], None]) -> None:
    """Builds fsr4_411/ from `dll`; raises SetupError with a message for the player."""
    tools = tools_dir(paths)
    if not tools_present(paths):
        raise SetupError("The FSR 4.1.1 tools are missing (bin/fsr411): reinstall DCport FSR")
    work = paths.root / "fsr411-work"
    shutil.rmtree(work, ignore_errors=True)
    run = work / "run"
    run.mkdir(parents=True)
    shutil.copyfile(tools / "fsr4cap.exe", run / "fsr4cap.exe")
    shutil.copyfile(dll, run / "amd_fidelityfx_upscaler_dx12.dll")
    _download(*LOADER, run / LOADER[0], report)

    for size in SIZES:
        report(f"Running FSR 4.1.1 once at {size} to record it…")
        try:
            p = subprocess.run([str(run / "fsr4cap.exe"), "4.1.1", size, size, "3"], cwd=run,
                               capture_output=True, text=True, timeout=300,
                               creationflags=NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise SetupError(f"The recording did not finish: {e}") from None
        log = run / "fsr4cap.log"  # the tool's own log; errors also go to its output
        text = p.stdout + p.stderr + (log.read_text(errors="replace") if log.is_file() else "")
        if "no version matching" in text:
            raise SetupError("This DLL offers no FSR 4.1.1 on this graphics card. AMD's own DLL "
                             "runs it on Radeon RX 9000 only; other cards need a DLL that "
                             "lifts that limit.")
        if p.returncode != 0 or "frames done" not in text:
            raise SetupError("The DLL could not be run: " + (text.strip().splitlines() or ["?"])[-1])

    report("Translating the shaders for Vulkan…")
    env_path = os.environ.get("PATH", "")
    os.environ["PATH"] = str(tools) + os.pathsep + env_path  # spirv-as, spirv-dis and their DLLs
    os.environ["FSR411_PORTABLE"] = "1"  # no VK_VALVE_shader_mixed_float_dot_product on Windows
    try:
        out = work / "fsr4_411"
        code, log = _script(tools / "extract.py", [str(tools / "dxil-spirv.exe"), str(run), str(out)])
        if code != 0:
            raise SetupError("The recording does not match what DCport expects:\n" + log[-600:])
        code, log = _script(tools / "name_bindings.py", ["--check", str(out)])
        if code == 3:
            report("Naming the shaders' resources (AMD's original DLL, read only)…")
            original = work / "original.dll"
            _download(ORIGINAL[0], ORIGINAL[1], original, report)
            code, log = _script(tools / "name_bindings.py",
                                [str(tools / "dxil-spirv.exe"), str(original), str(out)])
        if code != 0:
            raise SetupError("The shaders could not be prepared:\n" + log[-600:])
    finally:
        os.environ["PATH"] = env_path
        os.environ.pop("FSR411_PORTABLE", None)

    target = assets_dir(paths)
    old = paths.root / "fsr4_411.old"
    shutil.rmtree(old, ignore_errors=True)
    if target.exists():
        target.rename(old)
    shutil.move(str(out), str(target))
    shutil.rmtree(old, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    report("FSR 4.1.1 is ready.")
