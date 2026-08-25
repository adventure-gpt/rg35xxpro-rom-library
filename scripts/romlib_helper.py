#!/usr/bin/env python3
"""Safe archive installation and recoverable library removal for ROM Library."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import sys
import tempfile
import time
import uuid
import zipfile
from pathlib import Path

VERSION = "1.0.1"
RESERVE_BYTES = 64 * 1024 * 1024
MAX_ENTRIES = 4096
MAX_EXPANDED_BYTES = 16 * 1024 * 1024 * 1024

SYSTEM_EXTENSIONS = {
    "A2600": {".a26", ".bin", ".zip"},
    "A5200": {".a52", ".bin", ".zip"},
    "A7800": {".a78", ".bin", ".zip"},
    "AMIGA": {".adf", ".adz", ".dms", ".hdf", ".lha", ".zip"},
    "ATOMISWAVE": {".bin", ".lst", ".zip"},
    "C64": {".d64", ".d81", ".g64", ".prg", ".t64", ".tap", ".zip"},
    "DREAMCAST": {".cdi", ".chd", ".cue", ".gdi", ".bin", ".raw"},
    "FC": {".nes", ".unf", ".unif", ".zip"},
    "FDS": {".fds", ".zip"},
    "GB": {".gb", ".zip"},
    "GBA": {".gba", ".zip"},
    "GBC": {".gbc", ".zip"},
    "GG": {".gg", ".zip"},
    "LYNX": {".lnx", ".zip"},
    "MD": {".bin", ".gen", ".md", ".smd", ".zip"},
    "MDCD": {".bin", ".chd", ".cue", ".iso"},
    "MSX": {".cas", ".dsk", ".mx1", ".mx2", ".rom", ".zip"},
    "N64": {".n64", ".v64", ".z64", ".zip"},
    "NDS": {".nds", ".zip"},
    "NEOCD": {".bin", ".chd", ".cue"},
    "NEOGEO": {".zip"},
    "NGP": {".ngc", ".ngp", ".zip"},
    "PCE": {".pce", ".zip"},
    "PCECD": {".bin", ".chd", ".cue", ".iso"},
    "PS": {".bin", ".chd", ".cue", ".m3u", ".pbp"},
    "PSP": {".cso", ".iso", ".pbp"},
    "SATURN": {".bin", ".chd", ".cue", ".iso"},
    "SEGA32X": {".32x", ".bin", ".zip"},
    "SFC": {".fig", ".sfc", ".smc", ".zip"},
    "SMS": {".bin", ".sms", ".zip"},
    "VB": {".vb", ".vboy", ".zip"},
    "WS": {".ws", ".wsc", ".zip"},
}


class RomlibError(RuntimeError):
    pass


def emit(**payload: object) -> None:
    payload.setdefault("ok", True)
    payload.setdefault("version", VERSION)
    print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))


def resolved(path: str | Path) -> Path:
    return Path(path).expanduser().resolve(strict=False)


def ensure_under(path: str | Path, root: str | Path, *, exists: bool = False) -> Path:
    candidate = resolved(path)
    base = resolved(root)
    try:
        candidate.relative_to(base)
    except ValueError as exc:
        raise RomlibError(f"path escapes managed root: {candidate}") from exc
    if exists and not candidate.exists():
        raise RomlibError(f"path does not exist: {candidate}")
    return candidate


def safe_name(value: str, fallback: str = "game") -> str:
    value = value.replace("\\", "_").replace("/", "_")
    value = re.sub(r"[<>:\"|?*\x00-\x1f]", "_", value).strip(" .")
    value = re.sub(r"\s+", " ", value)
    if not value:
        value = fallback
    encoded = value.encode("utf-8")
    while len(encoded) > 180 and len(value) > 1:
        value = value[:-1]
        encoded = value.encode("utf-8")
    return value or fallback


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def unique_file(path: Path) -> Path:
    if not path.exists():
        return path
    for number in range(2, 1000):
        candidate = path.with_name(f"{path.stem} ({number}){path.suffix}")
        if not candidate.exists():
            return candidate
    raise RomlibError(f"too many filename conflicts for {path.name}")


def unique_dir(path: Path) -> Path:
    if not path.exists():
        return path
    for number in range(2, 1000):
        candidate = path.with_name(f"{path.name} ({number})")
        if not candidate.exists():
            return candidate
    raise RomlibError(f"too many folder conflicts for {path.name}")


def validate_zip_member(info: zipfile.ZipInfo) -> tuple[str, str]:
    raw = info.filename.replace("\\", "/")
    if "\x00" in raw or raw.startswith("/") or re.match(r"^[A-Za-z]:", raw):
        raise RomlibError(f"unsafe archive path: {info.filename}")
    parts = [part for part in raw.split("/") if part not in ("", ".")]
    if not parts or ".." in parts:
        raise RomlibError(f"unsafe archive path: {info.filename}")
    mode = (info.external_attr >> 16) & 0o170000
    if mode == stat.S_IFLNK:
        raise RomlibError(f"symbolic links are not allowed in archives: {info.filename}")
    leaf = safe_name(parts[-1])
    return raw, leaf


def copy_atomic(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.parent / f".{destination.name}.romlib-{uuid.uuid4().hex}.part"
    try:
        with source.open("rb") as src, temporary.open("xb") as dst:
            shutil.copyfileobj(src, dst, 1024 * 1024)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def install_archive(archive_arg: str, system: str, rom_root_arg: str, download_name: str) -> None:
    if system not in SYSTEM_EXTENSIONS:
        raise RomlibError(f"unsupported console mapping: {system}")
    archive = resolved(archive_arg)
    if not archive.is_file():
        raise RomlibError(f"download is missing: {archive}")
    rom_root = resolved(rom_root_arg)
    destination_root = ensure_under(rom_root / system, rom_root)
    destination_root.mkdir(parents=True, exist_ok=True)
    staging_root = ensure_under(rom_root.parent / ".rom-library-staging", rom_root.parent)
    staging_root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="install-", dir=staging_root))
    installed: list[dict[str, object]] = []
    skipped: list[str] = []
    allowed = SYSTEM_EXTENSIONS[system]
    try:
        candidates: list[tuple[str, str, int]] = []
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as bundle:
                infos = [entry for entry in bundle.infolist() if not entry.is_dir()]
                if len(infos) > MAX_ENTRIES:
                    raise RomlibError(f"archive contains too many files ({len(infos)})")
                expanded = sum(entry.file_size for entry in infos)
                if expanded > MAX_EXPANDED_BYTES:
                    raise RomlibError("archive expands beyond the 16 GB safety limit")
                free = shutil.disk_usage(destination_root).free
                if expanded + RESERVE_BYTES > free:
                    raise RomlibError("not enough ROM storage for the expanded archive")
                seen: set[str] = set()
                for entry in infos:
                    raw, leaf = validate_zip_member(entry)
                    extension = Path(leaf).suffix.lower()
                    if extension not in allowed:
                        continue
                    folded = leaf.casefold()
                    if folded in seen:
                        raise RomlibError(f"archive has duplicate output filenames: {leaf}")
                    seen.add(folded)
                    candidates.append((raw, leaf, entry.file_size))
                if not candidates:
                    expected = ", ".join(sorted(allowed))
                    raise RomlibError(f"archive contains no supported {system} files ({expected})")
                multi_file = len(candidates) > 1 or any(Path(leaf).suffix.lower() in {".cue", ".gdi", ".m3u"} for _, leaf, _ in candidates)
                target_base = destination_root
                if multi_file:
                    base = safe_name(Path(download_name or archive.name).stem, "game")
                    target_base = unique_dir(destination_root / base)
                for raw, leaf, expected_size in candidates:
                    staged = staging / leaf
                    with bundle.open(raw) as src, staged.open("xb") as dst:
                        shutil.copyfileobj(src, dst, 1024 * 1024)
                    if staged.stat().st_size != expected_size:
                        raise RomlibError(f"archive extraction size mismatch: {leaf}")
                    target = target_base / leaf
                    digest = sha256(staged)
                    if target.exists() and target.is_file() and target.stat().st_size == staged.stat().st_size and sha256(target) == digest:
                        skipped.append(str(target))
                        continue
                    if target.exists() and not multi_file:
                        target = unique_file(target)
                    elif target.exists():
                        raise RomlibError(f"multi-file destination already exists: {target}")
                    copy_atomic(staged, target)
                    installed.append({"path": str(target), "bytes": target.stat().st_size, "sha256": digest})
        else:
            leaf = safe_name(download_name or archive.name, "game")
            extension = Path(leaf).suffix.lower()
            if extension not in allowed:
                raise RomlibError(f"download is not ZIP and its extension is not supported for {system}: {extension or 'none'}")
            free = shutil.disk_usage(destination_root).free
            if archive.stat().st_size + RESERVE_BYTES > free:
                raise RomlibError("not enough ROM storage for this download")
            target = destination_root / leaf
            digest = sha256(archive)
            if target.exists() and target.is_file() and target.stat().st_size == archive.stat().st_size and sha256(target) == digest:
                skipped.append(str(target))
            else:
                if target.exists():
                    target = unique_file(target)
                copy_atomic(archive, target)
                installed.append({"path": str(target), "bytes": target.stat().st_size, "sha256": digest})
        if not installed and not skipped:
            raise RomlibError("installation produced no game files")
        emit(action="install", system=system, installed=installed, skipped=skipped)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def referenced_files(primary: Path, rom_root: Path) -> list[Path]:
    files = [primary]
    extension = primary.suffix.lower()
    references: list[str] = []
    try:
        text = primary.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return files
    if extension == ".cue":
        for line in text.splitlines():
            match = re.match(r"\s*FILE\s+(?:\"([^\"]+)\"|(\S+))", line, re.IGNORECASE)
            if match:
                references.append(match.group(1) or match.group(2))
    elif extension == ".m3u":
        references.extend(line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#"))
    elif extension == ".gdi":
        for line in text.splitlines()[1:]:
            try:
                fields = shlex.split(line)
            except ValueError:
                continue
            if len(fields) >= 5:
                references.append(fields[4])
    for reference in references:
        candidate = ensure_under(primary.parent / reference, rom_root)
        if candidate.is_file() and candidate not in files:
            files.append(candidate)
    return files


def trash_game(source_arg: str, rom_root_arg: str, trash_root_arg: str) -> None:
    rom_root = resolved(rom_root_arg)
    source = ensure_under(source_arg, rom_root, exists=True)
    if not source.is_file():
        raise RomlibError("only game files can be removed")
    trash_root = resolved(trash_root_arg)
    trash_root.mkdir(parents=True, exist_ok=True)
    group = ensure_under(trash_root / f"{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}", trash_root)
    files = referenced_files(source, rom_root)
    group.mkdir(parents=True, exist_ok=False)
    moved: list[dict[str, object]] = []
    try:
        for item in files:
            relative = item.relative_to(rom_root)
            destination = group / "files" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(item, destination)
            moved.append({"original": str(relative), "trash": str(destination.relative_to(group)), "bytes": destination.stat().st_size})
        manifest = {
            "version": 1,
            "created": int(time.time()),
            "primary": str(source.relative_to(rom_root)),
            "files": moved,
        }
        manifest_path = group / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        emit(action="trash", manifest=str(manifest_path), primary=manifest["primary"], files=moved)
    except Exception:
        for entry in reversed(moved):
            trashed = group / str(entry["trash"])
            original = rom_root / str(entry["original"])
            if trashed.exists() and not original.exists():
                original.parent.mkdir(parents=True, exist_ok=True)
                os.replace(trashed, original)
        shutil.rmtree(group, ignore_errors=True)
        raise


def load_manifest(manifest_arg: str, trash_root_arg: str) -> tuple[Path, dict[str, object], Path]:
    trash_root = resolved(trash_root_arg)
    manifest_path = ensure_under(manifest_arg, trash_root, exists=True)
    if manifest_path.name != "manifest.json" or not manifest_path.is_file():
        raise RomlibError("invalid trash manifest")
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if data.get("version") != 1 or not isinstance(data.get("files"), list):
        raise RomlibError("unsupported trash manifest")
    return manifest_path, data, manifest_path.parent


def list_trash(trash_root_arg: str) -> None:
    trash_root = resolved(trash_root_arg)
    items: list[dict[str, object]] = []
    if trash_root.is_dir():
        for manifest_path in sorted(trash_root.glob("*/manifest.json"), reverse=True):
            try:
                data = json.loads(manifest_path.read_text(encoding="utf-8"))
                files = data.get("files", [])
                total = sum(int(entry.get("bytes", 0)) for entry in files if isinstance(entry, dict))
                available = all((manifest_path.parent / str(entry.get("trash", ""))).is_file() for entry in files if isinstance(entry, dict))
                items.append({
                    "manifest": str(manifest_path),
                    "primary": str(data.get("primary", "Unknown game")),
                    "created": int(data.get("created", 0)),
                    "bytes": total,
                    "files": len(files),
                    "available": available,
                })
            except (OSError, ValueError, TypeError):
                continue
    emit(action="list-trash", items=items)


def remove_empty_parents(path: Path, stop: Path) -> None:
    current = path
    while current != stop and current.is_dir():
        try:
            current.rmdir()
        except OSError:
            break
        current = current.parent


def restore_game(manifest_arg: str, rom_root_arg: str, trash_root_arg: str) -> None:
    manifest_path, data, group = load_manifest(manifest_arg, trash_root_arg)
    rom_root = resolved(rom_root_arg)
    files = data["files"]
    destinations: list[tuple[Path, Path]] = []
    for entry in files:
        original = ensure_under(rom_root / str(entry["original"]), rom_root)
        trashed = ensure_under(group / str(entry["trash"]), group, exists=True)
        if original.exists():
            raise RomlibError(f"cannot restore because destination exists: {original}")
        destinations.append((trashed, original))
    restored: list[str] = []
    try:
        for trashed, original in destinations:
            original.parent.mkdir(parents=True, exist_ok=True)
            os.replace(trashed, original)
            restored.append(str(original))
        manifest_path.unlink()
        remove_empty_parents(group / "files", group)
        remove_empty_parents(group, resolved(trash_root_arg))
        emit(action="restore", restored=restored)
    except Exception:
        for trashed, original in reversed(destinations):
            if original.exists() and not trashed.exists():
                trashed.parent.mkdir(parents=True, exist_ok=True)
                os.replace(original, trashed)
        raise


def purge_game(manifest_arg: str, trash_root_arg: str) -> None:
    manifest_path, data, group = load_manifest(manifest_arg, trash_root_arg)
    removed = 0
    for entry in data["files"]:
        trashed = ensure_under(group / str(entry["trash"]), group)
        if trashed.is_file():
            removed += trashed.stat().st_size
            trashed.unlink()
    manifest_path.unlink(missing_ok=True)
    shutil.rmtree(group, ignore_errors=True)
    emit(action="purge", removedBytes=removed)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version=VERSION)
    sub = parser.add_subparsers(dest="command", required=True)

    install = sub.add_parser("install")
    install.add_argument("--archive", required=True)
    install.add_argument("--download-name", default="")
    install.add_argument("--system", required=True)
    install.add_argument("--rom-root", required=True)

    trash = sub.add_parser("trash")
    trash.add_argument("--source", required=True)
    trash.add_argument("--rom-root", required=True)
    trash.add_argument("--trash-root", required=True)

    listing = sub.add_parser("list-trash")
    listing.add_argument("--trash-root", required=True)

    restore = sub.add_parser("restore")
    restore.add_argument("--manifest", required=True)
    restore.add_argument("--rom-root", required=True)
    restore.add_argument("--trash-root", required=True)

    purge = sub.add_parser("purge")
    purge.add_argument("--manifest", required=True)
    purge.add_argument("--trash-root", required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "install":
            install_archive(args.archive, args.system, args.rom_root, args.download_name)
        elif args.command == "trash":
            trash_game(args.source, args.rom_root, args.trash_root)
        elif args.command == "list-trash":
            list_trash(args.trash_root)
        elif args.command == "restore":
            restore_game(args.manifest, args.rom_root, args.trash_root)
        elif args.command == "purge":
            purge_game(args.manifest, args.trash_root)
        else:
            raise RomlibError("unknown command")
        return 0
    except (RomlibError, OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        print(json.dumps({"ok": False, "version": VERSION, "error": str(exc)}, ensure_ascii=False, separators=(",", ":")))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
