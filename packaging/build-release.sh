#!/bin/sh
set -eu

VERSION=1.1.0
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME="RG35XXPro-ROM-Library-v$VERSION"
STAGE="$ROOT/build/release/$NAME"
ARCHIVE="$ROOT/dist/$NAME.tar.gz"

[ -x "$ROOT/build/rom-library" ] || { printf 'Build the application first.\n' >&2; exit 1; }
[ -f "$ROOT/assets/ROM Library.png" ] || { printf 'Missing release artwork.\n' >&2; exit 1; }
[ -f "$ROOT/third_party/7zip-arm64/7zzs" ] || { printf 'Missing ARM64 archive extractor.\n' >&2; exit 1; }

rm -rf "$ROOT/build/release"
mkdir -p "$STAGE/bin" "$STAGE/launcher" "$STAGE/artwork" "$STAGE/docs" "$STAGE/licenses/7zip" "$ROOT/dist"
cp "$ROOT/build/rom-library" "$STAGE/bin/rom-library"
cp "$ROOT/scripts/romlib_helper.py" "$STAGE/bin/romlib_helper.py"
cp "$ROOT/third_party/7zip-arm64/7zzs" "$STAGE/bin/7zzs"
cp "$ROOT/third_party/7zip-arm64/License.txt" "$STAGE/licenses/7zip/License.txt"
cp "$ROOT/third_party/7zip-arm64/readme.txt" "$STAGE/licenses/7zip/readme.txt"
cp "$ROOT/third_party/7zip-arm64/ORIGIN.md" "$STAGE/licenses/7zip/ORIGIN.md"
cp "$ROOT/packaging/ROM Library.sh" "$STAGE/launcher/ROM Library.sh"
cp "$ROOT/assets/ROM Library.png" "$STAGE/artwork/ROM Library.png"
cp "$ROOT/packaging/install.sh" "$STAGE/install.sh"
cp "$ROOT/packaging/uninstall.sh" "$STAGE/uninstall.sh"
cp "$ROOT/README.md" "$STAGE/README.md"
cp "$ROOT/docs/FORMATS.md" "$STAGE/docs/FORMATS.md"
cp "$ROOT/docs/INSTALLATION.md" "$STAGE/docs/INSTALLATION.md"
chmod 0755 "$STAGE/bin/rom-library" "$STAGE/bin/romlib_helper.py" "$STAGE/bin/7zzs" "$STAGE/launcher/ROM Library.sh" "$STAGE/install.sh" "$STAGE/uninstall.sh"
(cd "$STAGE" && sha256sum bin/rom-library bin/romlib_helper.py bin/7zzs 'launcher/ROM Library.sh' 'artwork/ROM Library.png' licenses/7zip/License.txt licenses/7zip/readme.txt licenses/7zip/ORIGIN.md install.sh uninstall.sh README.md docs/FORMATS.md docs/INSTALLATION.md > SHA256SUMS)
rm -f "$ARCHIVE" "$ARCHIVE.sha256"
tar -C "$ROOT/build/release" -czf "$ARCHIVE" "$NAME"
(cd "$ROOT/dist" && sha256sum "$(basename "$ARCHIVE")" > "$(basename "$ARCHIVE").sha256")
printf '%s\n' "$ARCHIVE"
