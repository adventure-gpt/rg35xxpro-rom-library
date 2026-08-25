#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts" / "romlib_helper.py"


def extractor() -> Path:
    configured = os.environ.get("ROM_LIBRARY_7ZZ")
    choices = [configured] if configured else []
    choices.extend(
        [
            shutil.which("7zz"),
            shutil.which("7z"),
            ROOT / "third_party" / "7zip-arm64" / "7zzs",
            Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "7-Zip" / "7z.exe",
        ]
    )
    for choice in choices:
        if choice and Path(choice).is_file():
            return Path(choice)
    raise AssertionError("7-Zip test runtime not found")


EXTRACTOR = extractor()


def run(*arguments: str, expected: int = 0) -> dict:
    environment = os.environ.copy()
    environment["ROM_LIBRARY_7ZZ"] = str(EXTRACTOR)
    result = subprocess.run([sys.executable, str(HELPER), *arguments], text=True, capture_output=True, env=environment)
    if result.returncode != expected:
        raise AssertionError(f"exit {result.returncode}, expected {expected}: {result.stdout} {result.stderr}")
    return json.loads(result.stdout)


def make_7z(archive: Path, source: Path, *names: str) -> None:
    result = subprocess.run(
        [str(EXTRACTOR), "a", "-t7z", "-mx=1", "-y", str(archive), *names],
        cwd=source,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise AssertionError(f"7z fixture creation failed: {result.stdout} {result.stderr}")


def load_helper_module():
    spec = importlib.util.spec_from_file_location("romlib_helper", HELPER)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    helper = load_helper_module()
    assert helper.VERSION == "1.1.0"
    assert helper.SYSTEM_EXTENSIONS["PSP"] == {".chd", ".cso", ".iso", ".pbp", ".prx"}
    assert "PS2" not in helper.SYSTEM_EXTENSIONS
    assert len(helper.SYSTEM_EXTENSIONS) >= 50

    with tempfile.TemporaryDirectory(prefix="romlib-tests-") as temporary:
        root = Path(temporary)
        roms = root / "Roms"
        trash = root / "Trash"
        downloads = root / "Downloads"
        (roms / "GBA").mkdir(parents=True)
        downloads.mkdir()

        payload = b"fixture-rom-data" * 4096
        archive = downloads / "fixture.bin"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as bundle:
            with bundle.open("Fixture Game.gba", "w", force_zip64=True) as member:
                member.write(payload)
            bundle.writestr("README.txt", "ignored")
        installed = run("install", "--archive", str(archive), "--download-name", "Fixture Game.zip", "--system", "GBA", "--rom-root", str(roms))
        game = Path(installed["installed"][0]["path"])
        assert game.read_bytes() == payload and installed["packageFormats"] == ["ZIP"]

        duplicate = run("install", "--archive", str(archive), "--download-name", "Fixture Game.zip", "--system", "GBA", "--rom-root", str(roms))
        assert duplicate["skipped"] and len(list((roms / "GBA").glob("*.gba"))) == 1

        direct_iso = downloads / "direct.payload"
        direct_iso.write_bytes(b"\0" * 0x8001 + b"CD001" + b"\0" * 4096)
        direct = run("install", "--archive", str(direct_iso), "--download-name", "Direct PSP Game.zip", "--system", "PSP", "--rom-root", str(roms))
        direct_target = Path(direct["installed"][0]["path"])
        assert direct_target.suffix.lower() == ".iso" and direct_target.read_bytes() == direct_iso.read_bytes()

        seven_source = root / "seven-source"
        seven_source.mkdir()
        (seven_source / "Size Matters.iso").write_bytes(b"psp-iso-fixture")
        disguised_7z = downloads / "provider-says-zip.payload"
        make_7z(disguised_7z, seven_source, "Size Matters.iso")
        seven = run("install", "--archive", str(disguised_7z), "--download-name", "Size Matters.zip", "--system", "PSP", "--rom-root", str(roms))
        assert seven["packageFormats"] == ["7Z"]
        assert Path(seven["installed"][0]["path"]).read_bytes() == b"psp-iso-fixture"

        inner = downloads / "inner.zip"
        with zipfile.ZipFile(inner, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("Nested.gba", b"nested-rom")
        outer = downloads / "outer.zip"
        with zipfile.ZipFile(outer, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.write(inner, "layers/inner.zip")
        nested = run("install", "--archive", str(outer), "--download-name", "Nested Package.zip", "--system", "GBA", "--rom-root", str(roms))
        assert Path(nested["installed"][0]["path"]).read_bytes() == b"nested-rom"

        tar_source = root / "tar-source"
        tar_source.mkdir()
        (tar_source / "Tarred.gba").write_bytes(b"tarred-rom")
        tar_gz = downloads / "tarred.weird"
        with tarfile.open(tar_gz, "w:gz") as bundle:
            bundle.add(tar_source / "Tarred.gba", arcname="Tarred.gba")
        tarred = run("install", "--archive", str(tar_gz), "--download-name", "Tarred Game.zip", "--system", "GBA", "--rom-root", str(roms))
        assert set(tarred["packageFormats"]) == {"GZ", "TAR"}
        assert Path(tarred["installed"][0]["path"]).read_bytes() == b"tarred-rom"

        disc = downloads / "disc.zip"
        cue_text = 'FILE "tracks/Track 01.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
        with zipfile.ZipFile(disc, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("Disc/Game.cue", cue_text)
            bundle.writestr("Disc/tracks/Track 01.bin", b"track-data")
            bundle.writestr("Disc/Game.sbi", b"sbi-data")
        disc_result = run("install", "--archive", str(disc), "--download-name", "Disc Game.zip", "--system", "PS", "--rom-root", str(roms))
        disc_paths = [Path(item["path"]) for item in disc_result["installed"]]
        disc_base = roms / "PS" / "Disc Game"
        assert {path.relative_to(disc_base).as_posix() for path in disc_paths} == {"Game.cue", "Game.sbi", "tracks/Track 01.bin"}
        disc_removed = run("trash", "--source", str(disc_base / "Game.cue"), "--rom-root", str(roms), "--trash-root", str(trash))
        assert len(disc_removed["files"]) == 3, disc_removed
        assert not (disc_base / "Game.cue").exists() and not (disc_base / "Game.sbi").exists() and not (disc_base / "tracks" / "Track 01.bin").exists()
        assert not disc_base.exists()
        run("restore", "--manifest", disc_removed["manifest"], "--rom-root", str(roms), "--trash-root", str(trash))
        assert (disc_base / "Game.cue").is_file() and (disc_base / "tracks" / "Track 01.bin").is_file()

        arcade = downloads / "arcade.zip"
        with zipfile.ZipFile(arcade, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("program.rom", b"arcade-program")
            bundle.writestr("sound.rom", b"arcade-sound")
        arcade_result = run("install", "--archive", str(arcade), "--download-name", "Arcade Set.zip", "--system", "NEOGEO", "--rom-root", str(roms))
        arcade_target = Path(arcade_result["installed"][0]["path"])
        assert arcade_target.read_bytes() == arcade.read_bytes() and zipfile.is_zipfile(arcade_target)

        raw_set = root / "raw-set"
        raw_set.mkdir()
        (raw_set / "program.bin").write_bytes(b"program")
        (raw_set / "sound.bin").write_bytes(b"sound")
        raw_7z = downloads / "raw-set.7z"
        make_7z(raw_7z, raw_set, "program.bin", "sound.bin")
        repacked = run("install", "--archive", str(raw_7z), "--download-name", "Raw CPS Set.7z", "--system", "CPS1", "--rom-root", str(roms))
        repacked_target = Path(repacked["installed"][0]["path"])
        with zipfile.ZipFile(repacked_target) as bundle:
            assert set(bundle.namelist()) == {"program.bin", "sound.bin"}

        html = downloads / "error.payload"
        html.write_text("<!doctype html><title>500 Internal Server Error</title>", encoding="utf-8")
        html_result = run("install", "--archive", str(html), "--download-name", "Broken.zip", "--system", "PSP", "--rom-root", str(roms), expected=2)
        assert "web error page" in html_result["error"]

        unsafe = downloads / "unsafe.zip"
        with zipfile.ZipFile(unsafe, "w") as bundle:
            bundle.writestr("../escape.gba", payload)
        rejected = run("install", "--archive", str(unsafe), "--download-name", "unsafe.zip", "--system", "GBA", "--rom-root", str(roms), expected=2)
        assert not rejected["ok"] and "unsafe archive path" in rejected["error"]
        assert not (root / "escape.gba").exists()

        linked = downloads / "linked.zip"
        link = zipfile.ZipInfo("linked.gba")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(linked, "w") as bundle:
            bundle.writestr(link, "../escape.gba")
        link_result = run("install", "--archive", str(linked), "--download-name", "linked.zip", "--system", "GBA", "--rom-root", str(roms), expected=2)
        assert "links are not allowed" in link_result["error"]

        removed = run("trash", "--source", str(game), "--rom-root", str(roms), "--trash-root", str(trash))
        assert not game.exists()
        listing = run("list-trash", "--trash-root", str(trash))
        assert len(listing["items"]) == 1 and listing["items"][0]["available"]
        run("restore", "--manifest", removed["manifest"], "--rom-root", str(roms), "--trash-root", str(trash))
        assert game.read_bytes() == payload

        removed = run("trash", "--source", str(game), "--rom-root", str(roms), "--trash-root", str(trash))
        purged = run("purge", "--manifest", removed["manifest"], "--trash-root", str(trash))
        assert purged["removedBytes"] == len(payload) and not game.exists()

    print(
        "HELPER_TEST formats=stock-matrix direct_iso=ok mislabeled_7z=ok nested_zip=ok "
        "tar_gz=ok disc_layout=ok arcade_zip=preserved arcade_7z=repacked html=rejected "
        "traversal=rejected symlink=rejected duplicate=ok trash_restore_purge=ok"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
