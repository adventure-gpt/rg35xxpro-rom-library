#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts" / "romlib_helper.py"


def run(*arguments: str, expected: int = 0) -> dict:
    result = subprocess.run([sys.executable, str(HELPER), *arguments], text=True, capture_output=True)
    if result.returncode != expected:
        raise AssertionError(f"exit {result.returncode}, expected {expected}: {result.stdout} {result.stderr}")
    return json.loads(result.stdout)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="romlib-tests-") as temporary:
        root = Path(temporary)
        roms = root / "Roms"
        trash = root / "Trash"
        downloads = root / "Downloads"
        (roms / "GBA").mkdir(parents=True)
        downloads.mkdir()

        archive = downloads / "fixture.bin"
        payload = b"fixture-rom-data" * 4096
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("Fixture Game.gba", payload)
            bundle.writestr("README.txt", "ignored")
        installed = run("install", "--archive", str(archive), "--download-name", "Fixture Game.zip", "--system", "GBA", "--rom-root", str(roms))
        game = Path(installed["installed"][0]["path"])
        assert game.read_bytes() == payload

        duplicate = run("install", "--archive", str(archive), "--download-name", "Fixture Game.zip", "--system", "GBA", "--rom-root", str(roms))
        assert duplicate["skipped"] and len(list((roms / "GBA").glob("*.gba"))) == 1

        removed = run("trash", "--source", str(game), "--rom-root", str(roms), "--trash-root", str(trash))
        assert not game.exists()
        listing = run("list-trash", "--trash-root", str(trash))
        assert len(listing["items"]) == 1 and listing["items"][0]["available"]
        run("restore", "--manifest", removed["manifest"], "--rom-root", str(roms), "--trash-root", str(trash))
        assert game.read_bytes() == payload

        removed = run("trash", "--source", str(game), "--rom-root", str(roms), "--trash-root", str(trash))
        purged = run("purge", "--manifest", removed["manifest"], "--trash-root", str(trash))
        assert purged["removedBytes"] == len(payload) and not game.exists()

        unsafe = downloads / "unsafe.zip"
        with zipfile.ZipFile(unsafe, "w") as bundle:
            bundle.writestr("../escape.gba", payload)
        rejected = run("install", "--archive", str(unsafe), "--download-name", "unsafe.zip", "--system", "GBA", "--rom-root", str(roms), expected=2)
        assert not rejected["ok"] and "unsafe archive path" in rejected["error"]
        assert not (root / "escape.gba").exists()

    print("HELPER_TEST install=ok duplicate=ok trash=ok restore=ok purge=ok zip_slip=rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
