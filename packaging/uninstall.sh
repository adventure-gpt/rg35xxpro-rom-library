#!/bin/sh
set -eu

APP_ROOT=/mnt/data/rom-library
LAUNCHER='/mnt/mmc/Roms/APPS/ROM Library.sh'
ARTWORK='/mnt/mmc/Roms/APPS/Imgs/ROM Library.png'

[ "$(id -u)" = 0 ] || { printf 'ERROR: Run as root.\n' >&2; exit 1; }
rm -f "$APP_ROOT/bin/rom-library" "$APP_ROOT/bin/romlib_helper.py" "$LAUNCHER" "$ARTWORK"
sync
printf 'ROM Library executables, launcher, and artwork were removed.\n'
printf 'Games, settings, logs, retained downloads, and recoverable trash were preserved.\n'
