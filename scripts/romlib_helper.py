#!/usr/bin/env python3
"""Safe, content-aware game installation and recoverable library removal."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

VERSION = "1.1.0"
RESERVE_BYTES = 64 * 1024 * 1024
MAX_ENTRIES = 8192
MAX_EXPANDED_BYTES = 16 * 1024 * 1024 * 1024
MAX_ARCHIVE_DEPTH = 4
MAX_COMPRESSION_RATIO = 1000
MIN_RATIO_ALLOWANCE = 64 * 1024 * 1024

# This table is derived from the RG35XX Pro stock launcher's own file filters.
# Descriptor/track sidecars are added separately so multi-file disc sets remain
# intact even when the launcher does not show every component in its browser.
SYSTEM_EXTENSIONS = {
    "A2600": {".a26", ".bin", ".zip"},
    "A5200": {".a52", ".zip"},
    "A7800": {".a78", ".bin", ".zip"},
    "A800": {".atr", ".rom", ".zip"},
    "AMIGA": {".adf", ".adz", ".chd", ".cue", ".dms", ".hdf", ".hdz", ".ipf", ".iso", ".lha", ".m3u", ".uae", ".zip"},
    "ATARIST": {".dim", ".ipf", ".m3u", ".msa", ".st", ".stx", ".zip"},
    "ATOMISWAVE": {".7z", ".bin", ".chd", ".cue", ".dat", ".gdi", ".iso", ".lst", ".zip"},
    "C64": {".bin", ".cmd", ".crt", ".d64", ".d6z", ".d71", ".d7z", ".d80", ".d81", ".d82", ".d8z", ".g41", ".g4z", ".g64", ".g6z", ".m3u", ".nbz", ".nib", ".p00", ".prg", ".t64", ".tap", ".vsf", ".x64", ".x6z", ".zip"},
    "CPS1": {".zip"},
    "CPS2": {".zip"},
    "CPS3": {".zip"},
    "DOS": {".bat", ".com", ".dosz", ".exe", ".zip"},
    "DREAMCAST": {".bin", ".cdi", ".chd", ".cue", ".gdi", ".iso", ".m3u", ".zip"},
    "EASYRPG": {".ldb", ".zip"},
    "FBNEO": {".zip"},
    "FC": {".nes", ".unf", ".unif", ".zip"},
    "FDS": {".fds", ".zip"},
    "GB": {".gb", ".zip"},
    "GBA": {".gba", ".zip"},
    "GBC": {".gbc", ".zip"},
    "GG": {".gg", ".zip"},
    "GW": {".mgw"},
    "HBMAME": {".chd", ".zip"},
    "JAVA": {".jar"},
    "LYNX": {".lnx", ".zip"},
    "MAME": {".chd", ".zip"},
    "MD": {".bin", ".gen", ".md", ".smd", ".zip"},
    "MDCD": {".chd", ".cue", ".iso", ".m3u", ".sg", ".zip"},
    "MSX": {".cas", ".col", ".dsk", ".m3u", ".mx1", ".mx2", ".ri", ".rom", ".sc", ".sg", ".zip"},
    "N64": {".bin", ".n64", ".rom", ".v64", ".z64", ".zip"},
    "NAOMI": {".7z", ".bin", ".chd", ".cue", ".dat", ".gdi", ".iso", ".zip"},
    "NDS": {".nds", ".zip"},
    "NEOCD": {".chd", ".cue", ".iso", ".zip"},
    "NEOGEO": {".zip"},
    "NGP": {".ngc", ".ngp", ".zip"},
    "ONS": {".dat", ".nt", ".nt2", ".nt3", ".ons", ".txt", ".zip"},
    "OPENBOR": {".pak"},
    "PCE": {".bin", ".ccd", ".chd", ".cue", ".img", ".iso", ".pce", ".zip"},
    "PCECD": {".ccd", ".chd", ".cue", ".m3u", ".toc"},
    "PGM2": {".zip"},
    "PICO": {".p8", ".png"},
    "POKE": {".min", ".zip"},
    "PS": {".bin", ".cbn", ".chd", ".cue", ".img", ".iso", ".m3u", ".mdf", ".mds", ".pbp", ".toc"},
    "PSP": {".chd", ".cso", ".iso", ".pbp", ".prx"},
    "SATURN": {".bin", ".ccd", ".chd", ".cue", ".iso", ".mds", ".rar"},
    "SCUMMVM": {".scummvm", ".zip"},
    "SEGA32X": {".32x", ".bin", ".md", ".smd", ".zip"},
    "SFC": {".fig", ".sfc", ".smc", ".zip"},
    "SMS": {".bin", ".sms", ".zip"},
    "VB": {".vb", ".vboy", ".zip"},
    "VARCADE": {".zip"},
    "VIC20": {".20", ".a0", ".b0", ".bin", ".cmd", ".crt", ".d6", ".d7", ".d8", ".g4", ".g6", ".gz", ".m3u", ".nbz", ".nib", ".p00", ".prg", ".t64", ".tap", ".vsf", ".x6", ".zip"},
    "WS": {".ws", ".wsc", ".zip"},
}

DISC_SYSTEMS = {"AMIGA", "DREAMCAST", "MDCD", "NAOMI", "NEOCD", "PCE", "PCECD", "PS", "PSP", "SATURN"}
DISC_SIDECARS = {".bin", ".ccd", ".cue", ".gdi", ".img", ".m3u", ".mdf", ".mds", ".raw", ".sbi", ".sub", ".toc", ".wav"}
ARCHIVE_ROM_SYSTEMS = {"ATOMISWAVE", "CPS1", "CPS2", "CPS3", "DOS", "FBNEO", "HBMAME", "MAME", "NAOMI", "NEOGEO", "ONS", "PGM2", "SCUMMVM", "VARCADE"}
PRESERVED_ARCHIVES = {"AMIGA": {".lha"}, "ATOMISWAVE": {".7z"}, "NAOMI": {".7z"}, "SATURN": {".rar"}}
ARCHIVE_KINDS = {".7z", ".arj", ".bz2", ".cab", ".cpio", ".gz", ".lha", ".lzh", ".rar", ".tar", ".xz", ".zip", ".zst"}
COMPOUND_ARCHIVE_SUFFIXES = (".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tbz2", ".tgz", ".txz", ".tzst", ".zip.001", ".7z.001")


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


def validate_member_path(value: str) -> Path:
    raw = value.replace("\\", "/")
    if "\x00" in raw or raw.startswith("/") or re.match(r"^[A-Za-z]:", raw):
        raise RomlibError(f"unsafe archive path: {value}")
    parts = [part for part in raw.split("/") if part not in ("", ".")]
    if not parts or ".." in parts:
        raise RomlibError(f"unsafe archive path: {value}")
    for part in parts:
        if re.search(r"[<>:\"|?*\x00-\x1f]", part) or len(part.encode("utf-8")) > 240:
            raise RomlibError(f"archive filename is not supported by ROM storage: {value}")
    return Path(*parts)


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


@dataclass
class Candidate:
    source: Path
    logical: Path


@dataclass
class ExtractionBudget:
    entries: int = 0
    expanded: int = 0
    formats: set[str] = field(default_factory=set)

    def add(self, entries: int, expanded: int, packed: int, kind: str) -> None:
        self.entries += entries
        self.expanded += expanded
        self.formats.add(kind.lstrip(".").upper())
        if self.entries > MAX_ENTRIES:
            raise RomlibError(f"nested package contains too many files ({self.entries})")
        if self.expanded > MAX_EXPANDED_BYTES:
            raise RomlibError("nested package expands beyond the 16 GB safety limit")
        if expanded > max(MIN_RATIO_ALLOWANCE, packed * MAX_COMPRESSION_RATIO):
            raise RomlibError("archive compression ratio exceeds the safety limit")


def seven_zip_path() -> Path:
    choices: list[str | Path] = []
    configured = os.environ.get("ROM_LIBRARY_7ZZ")
    if configured:
        choices.append(configured)
    choices.extend((Path(__file__).resolve().with_name("7zzs"), "7zz", "7z", "7za"))
    for choice in choices:
        path = Path(choice) if os.path.sep in str(choice) or Path(str(choice)).is_absolute() else Path(shutil.which(str(choice)) or "")
        if str(path) and path.is_file():
            return path
    raise RomlibError("the bundled archive extractor is missing; reinstall ROM Library")


def compound_suffix(name: str) -> str:
    lowered = name.lower()
    for suffix in COMPOUND_ARCHIVE_SUFFIXES:
        if lowered.endswith(suffix):
            return suffix
    return Path(lowered).suffix


def without_archive_suffix(name: str) -> str:
    suffix = compound_suffix(name)
    return name[: -len(suffix)] if suffix else name


def content_kind(path: Path) -> str:
    with path.open("rb") as handle:
        head = handle.read(64)
        marker = b""
        tar_marker = b""
        if path.stat().st_size >= 262:
            handle.seek(257)
            tar_marker = handle.read(5)
        if path.stat().st_size >= 0x8006:
            handle.seek(0x8001)
            marker = handle.read(5)
    if head.startswith((b"PK\x03\x04", b"PK\x05\x06", b"PK\x06\x06", b"PK\x07\x08")):
        return ".zip"
    if head.startswith(b"7z\xbc\xaf'\x1c"):
        return ".7z"
    if head.startswith((b"Rar!\x1a\x07\x00", b"Rar!\x1a\x07\x01\x00")):
        return ".rar"
    if head.startswith(b"\x1f\x8b\x08"):
        return ".gz"
    if head.startswith(b"BZh"):
        return ".bz2"
    if head.startswith(b"\xfd7zXZ\x00"):
        return ".xz"
    if head.startswith(b"(\xb5/\xfd"):
        return ".zst"
    if head.startswith(b"MSCF\x00\x00\x00\x00"):
        return ".cab"
    if head.startswith(b"\x60\xea"):
        return ".arj"
    if len(head) >= 7 and head[2:5] == b"-lh":
        return ".lha"
    if head.startswith((b"070701", b"070702", b"070707")):
        return ".cpio"
    if tar_marker == b"ustar":
        return ".tar"
    if marker == b"CD001":
        return ".iso"
    if head.startswith((b"CISO", b"ZISO")):
        return ".cso"
    if head.startswith(b"MComprHD"):
        return ".chd"
    if head.startswith(b"\x00PBP"):
        return ".pbp"
    if head.startswith(b"NES\x1a"):
        return ".nes"
    if head.startswith(b"\x80\x37\x12\x40"):
        return ".z64"
    if head.startswith(b"\x37\x80\x40\x12"):
        return ".v64"
    if head.startswith(b"\x40\x12\x37\x80"):
        return ".n64"
    return ""


def looks_like_error_page(path: Path) -> bool:
    with path.open("rb") as handle:
        sample = handle.read(4096).lstrip().lower()
    return sample.startswith((b"<!doctype html", b"<html", b"<?xml")) or (sample.startswith(b"{") and b'"error"' in sample)


def list_archive(extractor: Path, archive: Path, budget: ExtractionBudget) -> None:
    result = subprocess.run(
        [str(extractor), "l", "-slt", "-ba", "-sccUTF-8", "--", str(archive)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )
    if result.returncode != 0:
        detail = next((line.strip() for line in result.stdout.splitlines() if "ERROR" in line.upper()), "damaged, unsupported, split, or password-protected archive")
        raise RomlibError(f"cannot inspect archive: {detail}")
    entries = 0
    expanded = 0
    seen: set[str] = set()
    for block in re.split(r"\r?\n\s*\r?\n", result.stdout.strip()):
        fields: dict[str, str] = {}
        for line in block.splitlines():
            if " = " in line:
                key, value = line.split(" = ", 1)
                fields[key.strip()] = value.strip()
        name = fields.get("Path")
        if not name or fields.get("Folder") == "+":
            continue
        relative = validate_member_path(name)
        folded = relative.as_posix().casefold()
        if folded in seen:
            raise RomlibError(f"archive has duplicate output paths: {name}")
        seen.add(folded)
        attributes = fields.get("Attributes", "")
        if fields.get("Symbolic Link") or fields.get("Hard Link") or re.search(r"(^|\s)l[rwx-]", attributes):
            raise RomlibError(f"links are not allowed in archives: {name}")
        if fields.get("Encrypted") == "+":
            raise RomlibError("password-protected archives are not supported")
        try:
            size = int(fields.get("Size", "0"))
        except ValueError as exc:
            raise RomlibError(f"archive reports an invalid file size: {name}") from exc
        if size < 0:
            raise RomlibError(f"archive reports an invalid file size: {name}")
        expanded += size
        entries += 1
    if entries == 0:
        raise RomlibError("archive contains no files")
    budget.add(entries, expanded, max(1, archive.stat().st_size), content_kind(archive) or compound_suffix(archive.name) or "archive")


def extract_archive(extractor: Path, archive: Path, destination: Path, budget: ExtractionBudget) -> None:
    list_archive(extractor, archive, budget)
    if budget.expanded + RESERVE_BYTES > shutil.disk_usage(destination.parent).free:
        raise RomlibError("not enough ROM storage for the expanded package")
    destination.mkdir(parents=True, exist_ok=False)
    result = subprocess.run(
        [str(extractor), "x", "-y", "-aoa", "-spe", "-snld0", "-bb0", "-bso0", "-bsp0", f"-o{destination}", "--", str(archive)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60 * 60,
    )
    if result.returncode != 0:
        detail = next((line.strip() for line in result.stdout.splitlines() if "ERROR" in line.upper()), "damaged, unsupported, split, or password-protected archive")
        raise RomlibError(f"archive extraction failed: {detail}")
    actual_entries = 0
    actual_bytes = 0
    for root, directories, files in os.walk(destination, followlinks=False):
        for name in directories + files:
            item = Path(root) / name
            relative = item.relative_to(destination)
            validate_member_path(relative.as_posix())
            mode = os.lstat(item).st_mode
            if stat.S_ISLNK(mode) or (not stat.S_ISDIR(mode) and not stat.S_ISREG(mode)):
                raise RomlibError(f"links and special files are not allowed in archives: {relative}")
            if stat.S_ISREG(mode):
                actual_entries += 1
                actual_bytes += item.stat().st_size
    if actual_entries == 0:
        raise RomlibError("archive extraction produced no files")
    if actual_entries > MAX_ENTRIES or actual_bytes > MAX_EXPANDED_BYTES:
        raise RomlibError("archive extraction exceeded the safety limit")


def archive_should_be_preserved(system: str, kind: str) -> bool:
    return (system in ARCHIVE_ROM_SYSTEMS and kind == ".zip") or kind in PRESERVED_ARCHIVES.get(system, set())


def repack_as_zip(source_root: Path, destination: Path) -> None:
    files = [item for item in sorted(source_root.rglob("*"), key=lambda value: value.as_posix().casefold()) if item.is_file()]
    if not files:
        raise RomlibError("archive ROM set contains no files")
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as bundle:
        for item in files:
            relative = item.relative_to(source_root)
            validate_member_path(relative.as_posix())
            bundle.write(item, relative.as_posix())


def collect_candidates(
    path: Path,
    logical: Path,
    system: str,
    extractor: Path,
    staging: Path,
    budget: ExtractionBudget,
    candidates: list[Candidate],
    depth: int = 0,
) -> None:
    kind = content_kind(path)
    suffix = compound_suffix(logical.name)
    if kind in ARCHIVE_KINDS:
        if archive_should_be_preserved(system, kind):
            target_name = logical.name
            if suffix != kind:
                target_name = without_archive_suffix(target_name) + kind
            candidates.append(Candidate(path, logical.with_name(target_name)))
            return
        if depth >= MAX_ARCHIVE_DEPTH:
            raise RomlibError(f"nested archive depth exceeds {MAX_ARCHIVE_DEPTH}: {logical}")
        expanded = staging / f"expanded-{depth}-{uuid.uuid4().hex}"
        extract_archive(extractor, path, expanded, budget)
        previous_count = len(candidates)
        for item in sorted(expanded.rglob("*"), key=lambda value: value.as_posix().casefold()):
            if not item.is_file() or any(parent != expanded and parent.name.startswith("expanded-") for parent in item.parents):
                continue
            relative = item.relative_to(expanded)
            collect_candidates(item, logical.parent / relative, system, extractor, staging, budget, candidates, depth + 1)
        if system in ARCHIVE_ROM_SYSTEMS and len(candidates) == previous_count:
            repacked = staging / f"repacked-{uuid.uuid4().hex}.zip"
            repack_as_zip(expanded, repacked)
            candidates.append(Candidate(repacked, logical.with_name(without_archive_suffix(logical.name) + ".zip")))
        return

    extension = Path(logical.name).suffix.lower()
    allowed = SYSTEM_EXTENSIONS[system]
    sidecars = DISC_SIDECARS if system in DISC_SYSTEMS else set()
    if kind in allowed and (extension in ARCHIVE_KINDS or extension not in allowed):
        logical = logical.with_name(without_archive_suffix(logical.name) + kind)
        extension = kind
    if extension in allowed or extension in sidecars:
        candidates.append(Candidate(path, logical))


def common_parent(paths: list[Path]) -> Path:
    if not paths:
        return Path()
    parents = [path.parent.parts for path in paths]
    shared: list[str] = []
    for parts in zip(*parents):
        if len({part.casefold() for part in parts}) != 1:
            break
        shared.append(parts[0])
    return Path(*shared)


def install_candidates(candidates: list[Candidate], destination_root: Path, download_name: str, staging: Path) -> tuple[list[dict[str, object]], list[str]]:
    installed: list[dict[str, object]] = []
    skipped: list[str] = []
    unique: list[Candidate] = []
    seen_sources: set[Path] = set()
    for candidate in candidates:
        source = candidate.source.resolve()
        if source not in seen_sources:
            seen_sources.add(source)
            unique.append(candidate)
    candidates = unique
    if len(candidates) == 1:
        candidate = candidates[0]
        target = destination_root / safe_name(candidate.logical.name)
        digest = sha256(candidate.source)
        if target.is_file() and target.stat().st_size == candidate.source.stat().st_size and sha256(target) == digest:
            return installed, [str(target)]
        if target.exists():
            target = unique_file(target)
        copy_atomic(candidate.source, target)
        return [{"path": str(target), "bytes": target.stat().st_size, "sha256": digest}], skipped

    wrapper = common_parent([candidate.logical for candidate in candidates])
    relative_items: list[tuple[Candidate, Path]] = []
    seen_outputs: set[str] = set()
    for candidate in candidates:
        relative = candidate.logical.relative_to(wrapper) if wrapper.parts else candidate.logical
        validate_member_path(relative.as_posix())
        folded = relative.as_posix().casefold()
        if folded in seen_outputs:
            raise RomlibError(f"package has duplicate output paths: {relative}")
        seen_outputs.add(folded)
        relative_items.append((candidate, relative))
    base = safe_name(without_archive_suffix(Path(download_name).name), "game")
    target_base = destination_root / base
    if target_base.is_dir() and all(
        (target_base / relative).is_file()
        and (target_base / relative).stat().st_size == candidate.source.stat().st_size
        and sha256(target_base / relative) == sha256(candidate.source)
        for candidate, relative in relative_items
    ):
        return installed, [str(target_base / relative) for _, relative in relative_items]
    target_base = unique_dir(target_base)
    tree = staging / f"final-{uuid.uuid4().hex}"
    tree.mkdir()
    records: list[tuple[Path, str]] = []
    for candidate, relative in relative_items:
        staged = tree / relative
        copy_atomic(candidate.source, staged)
        records.append((relative, sha256(staged)))
    os.replace(tree, target_base)
    for relative, digest in records:
        target = target_base / relative
        installed.append({"path": str(target), "bytes": target.stat().st_size, "sha256": digest})
    return installed, skipped


def install_archive(archive_arg: str, system: str, rom_root_arg: str, download_name: str) -> None:
    if system not in SYSTEM_EXTENSIONS:
        raise RomlibError(f"unsupported console mapping: {system}")
    archive = resolved(archive_arg)
    if not archive.is_file():
        raise RomlibError(f"download is missing: {archive}")
    if looks_like_error_page(archive):
        raise RomlibError("provider returned a web error page instead of a game file")
    rom_root = resolved(rom_root_arg)
    destination_root = ensure_under(rom_root / system, rom_root)
    destination_root.mkdir(parents=True, exist_ok=True)
    staging_root = ensure_under(rom_root.parent / ".rom-library-staging", rom_root.parent)
    staging_root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="install-", dir=staging_root))
    try:
        if archive.stat().st_size + RESERVE_BYTES > shutil.disk_usage(destination_root).free:
            raise RomlibError("not enough ROM storage for this download")
        extractor = seven_zip_path()
        advertised = validate_member_path(safe_name(download_name or archive.name, "game")).name
        candidates: list[Candidate] = []
        budget = ExtractionBudget()
        collect_candidates(archive, Path(advertised), system, extractor, staging, budget, candidates)
        if not candidates:
            expected = ", ".join(sorted(SYSTEM_EXTENSIONS[system]))
            kind = content_kind(archive)
            detail = f"; detected {kind.lstrip('.').upper()} content" if kind else ""
            raise RomlibError(f"package contains no supported {system} game files ({expected}){detail}")
        installed, skipped = install_candidates(candidates, destination_root, advertised, staging)
        if not installed and not skipped:
            raise RomlibError("installation produced no game files")
        formats = sorted(budget.formats) or [content_kind(archive).lstrip(".").upper() or "DIRECT"]
        emit(action="install", system=system, packageFormats=formats, installed=installed, skipped=skipped)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def referenced_files(primary: Path, rom_root: Path) -> list[Path]:
    files: list[Path] = []
    pending = [primary]
    seen: set[Path] = set()
    while pending:
        current = pending.pop(0)
        if current in seen or not current.is_file():
            continue
        seen.add(current)
        files.append(current)
        for candidate in direct_references(current, rom_root):
            if candidate not in seen:
                pending.append(candidate)
    return files


def direct_references(primary: Path, rom_root: Path) -> list[Path]:
    extension = primary.suffix.lower()
    references: list[str] = []
    try:
        text = primary.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
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
    elif extension == ".toc":
        for line in text.splitlines():
            match = re.search(r"\b(?:FILE|DATAFILE|AUDIOFILE)\s+(?:\"([^\"]+)\"|(\S+))", line, re.IGNORECASE)
            if match:
                references.append(match.group(1) or match.group(2))
    elif extension == ".ccd":
        references.extend(primary.with_suffix(suffix).name for suffix in (".img", ".sub") if primary.with_suffix(suffix).is_file())
    elif extension == ".mds":
        if primary.with_suffix(".mdf").is_file():
            references.append(primary.with_suffix(".mdf").name)
    if extension in {".cue", ".ccd", ".mds", ".toc"} and primary.with_suffix(".sbi").is_file():
        references.append(primary.with_suffix(".sbi").name)
    files: list[Path] = []
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
        for item in sorted(files, key=lambda value: len(value.parts), reverse=True):
            remove_empty_parents(item.parent, rom_root)
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
