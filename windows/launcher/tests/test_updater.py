# SPDX-License-Identifier: GPL-2.0-or-later
"""Updates from GitHub releases (dcport/updater.py)."""
import hashlib
import io
import zipfile
from pathlib import Path

import pytest

from dcport import updater


def release(tag, names, draft=False, prerelease=False, digest=True):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease,
            "html_url": f"https://github.com/x/releases/{tag}",
            "assets": [{"name": n, "size": 10, "browser_download_url": f"https://dl/{n}",
                        "digest": "sha256:" + "AB" * 32 if digest else None} for n in names]}


def test_parse_version():
    assert updater.parse_version("0.3.5") == (0, 3, 5)
    assert updater.parse_version("0.10.0") > updater.parse_version("0.9.9")
    assert updater.parse_version("0.3.5-test") is None


def test_newest_picks_the_highest_release_with_this_editions_zip():
    rels = [release("windows-v0.3.6", ["DCport-0.3.6-windows.zip"]),       # no FSR zip
            release("windows-v0.3.7", ["DCport-0.3.7-windows-fsr.zip"], draft=True),
            release("windows-v0.3.8", ["DCport-0.3.8-windows-fsr.zip"], prerelease=True),
            release("windows-v0.3.5", ["DCport-0.3.5-windows-fsr.zip"]),
            release("other-v9.0.0", ["DCport-9.0.0-windows-fsr.zip"]),
            release("windows-v0.4.0", ["DCport-0.4.0-windows-fsr.zip",
                                       "DCport-0.4.0-windows.zip"])]
    r = updater.newest(rels, "0.3.5", fsr=True)
    assert r.version == "0.4.0" and r.url == "https://dl/DCport-0.4.0-windows-fsr.zip"
    assert r.sha256 == "ab" * 32 and r.size == 10
    assert updater.newest(rels, "0.3.5", fsr=False).version == "0.4.0"
    assert updater.newest(rels, "0.4.0", fsr=True) is None
    assert updater.newest(rels, "0.3.5-test", fsr=True).version == "0.4.0"
    assert updater.newest(rels, "0.4.0-test", fsr=True).version == "0.4.0"   # its final release
    assert updater.newest(rels, "0.4.1-test", fsr=True) is None              # never a downgrade
    assert updater.newest({"message": "rate limited"}, "0.3.5", fsr=True) is None


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def test_download_checks_size_and_sha256(tmp_path, monkeypatch):
    data = b"zip bytes"
    monkeypatch.setattr(updater.urllib.request, "urlopen", lambda req, timeout: FakeResponse(data))
    good = updater.Release("0.4.0", "https://dl/z", len(data), hashlib.sha256(data).hexdigest(), "")
    path = updater.download(good, tmp_path, fsr=True)
    assert path.name == "DCport-0.4.0-windows-fsr.zip" and path.read_bytes() == data
    with pytest.raises(ValueError, match="SHA-256"):
        updater.download(updater.Release("0.4.0", "https://dl/z", len(data), "00" * 32, ""),
                         tmp_path, fsr=True)
    with pytest.raises(ValueError, match="incomplete"):
        updater.download(updater.Release("0.4.0", "https://dl/z", 99, "", ""), tmp_path, fsr=True)


def make_zip(path: Path, files: dict) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for name, text in files.items():
            z.writestr(name, text)
    return path


def make_install(root: Path) -> None:
    (root / "bin").mkdir(parents=True)
    (root / "bin" / "DriveClub.exe").write_text("old runtime")
    (root / "bin" / "gone.dll").write_text("old dll")
    (root / "DriveClub.exe").write_text("old launcher")
    (root / "dcport.ini").write_text("upscaler=fsr411")
    (root / "input.ini").write_text("my keys")
    (root / "user" / "savedata").mkdir(parents=True)
    (root / "user" / "savedata" / "PROFILE").write_text("my save")
    (root / "fsr4_411").mkdir()
    (root / "data").mkdir()


RELEASE = {"DCport-0.4.0-windows-fsr/DriveClub.exe": "new launcher",
           "DCport-0.4.0-windows-fsr/bin/DriveClub.exe": "new runtime",
           "DCport-0.4.0-windows-fsr/dcport.ini": "upscaler=fsr4",
           "DCport-0.4.0-windows-fsr/input.ini": "default keys",
           "DCport-0.4.0-windows-fsr/README.txt": "new readme"}


def test_install_replaces_program_files_and_keeps_the_players(tmp_path):
    root = tmp_path / "DCport"
    make_install(root)
    new = updater.stage(make_zip(tmp_path / "u.zip", RELEASE), root)
    replaced = updater.install(new, root, running_exe=root / "DriveClub.exe")
    assert sorted(replaced) == ["DriveClub.exe", "README.txt", "bin"]
    assert (root / "DriveClub.exe").read_text() == "new launcher"
    assert (root / updater.OLD_EXE).read_text() == "old launcher"   # the running one, set aside
    assert (root / "bin" / "DriveClub.exe").read_text() == "new runtime"
    assert not (root / "bin" / "gone.dll").exists()                 # the whole bin is the release's
    assert (root / "dcport.ini").read_text() == "upscaler=fsr411"
    assert (root / "input.ini").read_text() == "my keys"
    assert (root / "user" / "savedata" / "PROFILE").read_text() == "my save"
    assert (root / "fsr4_411").is_dir() and (root / "data").is_dir()
    assert not (root / updater.OLD).exists() and not (root / updater.STAGE).exists()
    updater.clean_up(root)
    assert not (root / updater.OLD_EXE).exists()


def test_install_puts_everything_back_when_a_move_fails(tmp_path, monkeypatch):
    root = tmp_path / "DCport"
    make_install(root)
    new = updater.stage(make_zip(tmp_path / "u.zip", RELEASE), root)
    real_rename = Path.rename

    def rename(self, target):
        if self.name == "README.txt" and self.parent.name.startswith("DCport-0.4.0"):
            raise PermissionError("in use")
        return real_rename(self, target)

    monkeypatch.setattr(Path, "rename", rename)
    with pytest.raises(OSError):
        updater.install(new, root, running_exe=root / "DriveClub.exe")
    assert (root / "DriveClub.exe").read_text() == "old launcher"
    assert (root / "bin" / "DriveClub.exe").read_text() == "old runtime"
    assert (root / "bin" / "gone.dll").exists() and not (root / "README.txt").exists()


def test_stage_refuses_unsafe_or_incomplete_zips(tmp_path):
    root = tmp_path / "DCport"
    make_install(root)
    with pytest.raises(ValueError, match="unsafe"):
        updater.stage(make_zip(tmp_path / "a.zip", {"../evil.txt": "x"}), root)
    with pytest.raises(ValueError, match="no DriveClub.exe"):
        updater.stage(make_zip(tmp_path / "b.zip", {"DCport/bin/x.dll": "x",
                                                    "DCport/README.txt": "x"}), root)
