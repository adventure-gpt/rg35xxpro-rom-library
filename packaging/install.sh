#!/bin/sh
set -eu

VERSION=1.0.1
APP_ROOT=/mnt/data/rom-library
APP_BIN="$APP_ROOT/bin"
APPS_ROOT=/mnt/mmc/Roms/APPS
LAUNCHER="$APPS_ROOT/ROM Library.sh"
ARTWORK="$APPS_ROOT/Imgs/ROM Library.png"
PACKAGE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

fail() {
    printf 'ERROR: %s\n' "$1" >&2
    exit 1
}

[ "$(id -u)" = 0 ] || fail 'Run this installer as root.'
[ -r /mnt/vendor/oem/board.ini ] || fail 'Unable to identify this handheld.'
grep -q 'RG35xxPRO' /mnt/vendor/oem/board.ini || fail 'This package supports only the Anbernic RG35XX Pro stock firmware.'
[ -d /mnt/mmc/Roms ] || fail 'Stock ROM storage was not found at /mnt/mmc/Roms.'
command -v python3 >/dev/null 2>&1 || fail 'python3 is missing from this firmware.'
command -v unzip >/dev/null 2>&1 || fail 'unzip is missing from this firmware.'
command -v sha256sum >/dev/null 2>&1 || fail 'sha256sum is missing from this firmware.'

cd "$PACKAGE_DIR"
sha256sum -c SHA256SUMS || fail 'Package integrity verification failed.'
missing=$(ldd bin/rom-library 2>&1 | grep 'not found' || true)
[ -z "$missing" ] || fail "Firmware compatibility check failed: $missing"
python3 bin/romlib_helper.py --version | grep -qx "$VERSION" || fail 'Helper version check failed.'
bin/rom-library --version | grep -q "$VERSION" || fail 'Application version check failed.'

mkdir -p "$APP_BIN" "$APP_ROOT/logs" "$APPS_ROOT/Imgs"
stamp=$(date +%Y%m%d-%H%M%S)
backup="$APP_ROOT/backups/$stamp"
if [ -e "$APP_BIN/rom-library" ] || [ -e "$APP_BIN/romlib_helper.py" ] || [ -e "$LAUNCHER" ] || [ -e "$ARTWORK" ]; then
    mkdir -p "$backup"
    [ ! -e "$APP_BIN/rom-library" ] || cp -p "$APP_BIN/rom-library" "$backup/rom-library"
    [ ! -e "$APP_BIN/romlib_helper.py" ] || cp -p "$APP_BIN/romlib_helper.py" "$backup/romlib_helper.py"
    [ ! -e "$LAUNCHER" ] || cp -p "$LAUNCHER" "$backup/ROM Library.sh"
    [ ! -e "$ARTWORK" ] || cp -p "$ARTWORK" "$backup/ROM Library.png"
fi

tmp_bin="$APP_BIN/.rom-library.new.$$"
tmp_helper="$APP_BIN/.romlib-helper.new.$$"
cp bin/rom-library "$tmp_bin"
cp bin/romlib_helper.py "$tmp_helper"
chmod 0755 "$tmp_bin" "$tmp_helper"
mv -f "$tmp_bin" "$APP_BIN/rom-library"
mv -f "$tmp_helper" "$APP_BIN/romlib_helper.py"

tmp_launcher="$APPS_ROOT/.ROM-Library-launcher.new.$$"
tmp_artwork="$APPS_ROOT/Imgs/.ROM-Library-artwork.new.$$"
cp 'launcher/ROM Library.sh' "$tmp_launcher"
cp 'artwork/ROM Library.png' "$tmp_artwork"
chmod 0755 "$tmp_launcher"
chmod 0644 "$tmp_artwork"
mv -f "$tmp_launcher" "$LAUNCHER"
mv -f "$tmp_artwork" "$ARTWORK"

printf '%s\n' "$VERSION" > "$APP_ROOT/VERSION"
sync
printf 'ROM Library %s installed successfully.\n' "$VERSION"
printf 'Application: %s\n' "$APP_BIN/rom-library"
printf 'Launcher: %s\n' "$LAUNCHER"
printf 'Artwork: %s\n' "$ARTWORK"
printf 'Existing games, app settings, downloads, and recoverable trash were preserved.\n'
printf 'Refresh or reopen the APPS list to see ROM Library.\n'
