# SPDX-License-Identifier: GPL-2.0-or-later
import os
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class AppPaths:
    root: Path
    bin: Path
    runtime: Path
    prepare_tool: Path
    data: Path
    user: Path
    patches_xml: Path
    ini: Path
    input_ini: Path
    log: Path
    prepare_log: Path
    settings_json: Path
    fsr4_dir: Path
    backups: Path

    @classmethod
    def for_root(cls, root: Path) -> "AppPaths":
        root = Path(root)
        return cls(
            root=root,
            bin=root / "bin",
            runtime=root / "bin" / "DriveClub.exe",
            prepare_tool=root / "bin" / "dc-prepare.exe",
            data=root / "data",
            user=root / "user",
            patches_xml=root / "patches" / "Driveclub.xml",
            ini=root / "dcport.ini",
            input_ini=root / "input.ini",
            log=root / "last_run.log",
            prepare_log=root / "data" / "prepare.log",
            settings_json=root / "dcport-launcher.json",
            fsr4_dir=root / "fsr4_shaders",
            backups=root / "backups",
        )

    @classmethod
    def current(cls) -> "AppPaths":
        if getattr(sys, "frozen", False):
            root = Path(sys.executable).parent
        elif os.environ.get("DCPORT_ROOT"):
            root = Path(os.environ["DCPORT_ROOT"])
        else:
            root = Path.cwd()
        return cls.for_root(root)
