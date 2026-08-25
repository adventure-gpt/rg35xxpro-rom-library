#!/bin/sh
set -eu

VERSION=1.0.1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME="RG35XXPro-ROM-Library-v$VERSION"
STAGE="$ROOT/build/release/$NAME"
ARCHIVE="$ROOT/dist/$NAME.tar.gz"

[ -x "$ROOT/build/rom-library" ] || { printf 'Build the application first.\n' >&2; exit 1; }
[ -f "$ROOT/assets/ROM Library.png" ] || { printf 'Missing release artwork.\n' >&2; exit 1; }

rm -rf "$ROOT/build/release"
mkdir -p "$STAGE/bin" "$STAGE/launcher" "$STAGE/artwork" "$ROOT/dist"
cp "$ROOT/build/rom-library" "$STAGE/bin/rom-library"
cp "$ROOT/scripts/romlib_helper.py" "$STAGE/bin/romlib_helper.py"
cp "$ROOT/packaging/ROM Library.sh" "$STAGE/launcher/ROM Library.sh"
cp "$ROOT/assets/ROM Library.png" "$STAGE/artwork/ROM Library.png"
cp "$ROOT/packaging/install.sh" "$STAGE/install.sh"
cp "$ROOT/packaging/uninstall.sh" "$STAGE/uninstall.sh"
cp "$ROOT/README.md" "$STAGE/README.md"
chmod 0755 "$STAGE/bin/rom-library" "$STAGE/bin/romlib_helper.py" "$STAGE/launcher/ROM Library.sh" "$STAGE/install.sh" "$STAGE/uninstall.sh"
(cd "$STAGE" && sha256sum bin/rom-library bin/romlib_helper.py 'launcher/ROM Library.sh' 'artwork/ROM Library.png' install.sh uninstall.sh README.md > SHA256SUMS)
rm -f "$ARCHIVE" "$ARCHIVE.sha256"
tar -C "$ROOT/build/release" -czf "$ARCHIVE" "$NAME"
(cd "$ROOT/dist" && sha256sum "$(basename "$ARCHIVE")" > "$(basename "$ARCHIVE").sha256")
printf '%s\n' "$ARCHIVE"
