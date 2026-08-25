#!/bin/sh
export HOME=/root
export ROM_LIBRARY_HOME=/mnt/data/rom-library
export ROM_LIBRARY_ROMS_ROOT=/mnt/mmc/Roms
export ROM_LIBRARY_HELPER=/mnt/data/rom-library/bin/romlib_helper.py
export ROM_LIBRARY_DOWNLOAD_ROOT=/mnt/mmc/.rom-library-downloads
export ROM_LIBRARY_TRASH_ROOT=/mnt/mmc/.rom-library-trash
export LD_LIBRARY_PATH=/usr/lib:/mnt/vendor/lib
export SDL_VIDEODRIVER=mali
export SDL_NOMOUSE=1

APP=/mnt/data/rom-library/bin/rom-library
LOG=/mnt/data/rom-library/logs/launcher.log
mkdir -p /mnt/data/rom-library/logs

if [ ! -x "$APP" ] || [ ! -x "$ROM_LIBRARY_HELPER" ]; then
    printf '%s missing ROM Library executable or helper\n' "$(date -Iseconds)" >> "$LOG"
    exit 1
fi

cd /root || exit 1
printf '%s launch\n' "$(date -Iseconds)" >> "$LOG"
"$APP" >> "$LOG" 2>&1
status=$?
printf '%s exit_status=%s\n' "$(date -Iseconds)" "$status" >> "$LOG"
sync
exit "$status"
