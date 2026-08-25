# ROM Library for RG35XX Pro

ROM Library is a native, controller-first game browser and library manager for the Anbernic RG35XX Pro stock firmware. It searches the approved `romsgames.net` provider, displays game details, obtains the provider's normal download ticket, downloads over verified HTTPS, safely installs supported files in the stock ROM folders, and manages existing games with recoverable removal.

Website: [rg35xxpro-rom-library.bryguy4.chatgpt.site](https://rg35xxpro-rom-library.bryguy4.chatgpt.site)  
Latest release: [v1.1.0](https://github.com/adventure-gpt/rg35xxpro-rom-library/releases/tag/v1.1.0)

## What it does

- Controller keyboard and provider title search
- Console-aware install destinations under `/mnt/mmc/Roms`
- Download progress, cancellation, storage preflight, and provider size verification
- Content-aware ZIP, ZIP64, 7z, RAR/RAR5, tar, gzip, bzip2, xz, Zstandard, LHA, ARJ, CAB, and CPIO handling, including bounded nested packages and mislabeled downloads
- Stock-format coverage for every game system exposed by the RG35XX Pro launcher, including direct ISO, CSO, CHD, PBP, PRX, cartridge images, arcade ROM sets, and multi-file disc layouts
- Archive safety checks for traversal, links, special files, encryption, unsafe names, duplicate output paths, excessive depth, entry count, expanded size, and compression ratio
- Idempotent duplicate detection by SHA-256
- Installed-game index with system filters
- Recoverable removal, restoration, and explicit per-item permanent deletion
- On-device controller calibration
- No accounts, cookies, ads, analytics, credential storage, or background service

ROM Library never touches a game unless the user explicitly chooses an install, remove, restore, or permanent-delete action. Removal first moves the selected game and referenced disc-set files into `/mnt/mmc/.rom-library-trash`.

## Controls

| Control | Action |
| --- | --- |
| D-pad / stick | Navigate |
| A | Select / confirm |
| B | Back / cancel active transfer |
| X | Context action, delete text, refresh library, or permanent-delete prompt |
| Y | Shift on keyboard or refresh search |
| L / R | Symbols or change library filter |
| Start | Submit search from keyboard |
| Menu | Exit |

If the stock mapping differs, open **Settings > Calibrate controller**.

## Install

Copy the release archive and its adjacent `.sha256` file to the RG35XX Pro, verify it, then extract and run:

```sh
sha256sum -c RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256
tar -xzf RG35XXPro-ROM-Library-v1.1.0.tar.gz
cd RG35XXPro-ROM-Library-v1.1.0
sh install.sh
```

Refresh or reopen the **APPS** list and launch **ROM Library**. The installer checks the exact board identifier, package checksums, helper version, required commands, and shared-library compatibility before changing installed files. Updates back up the prior app files and preserve games and application data.

## Paths

- Program: `/mnt/data/rom-library/bin/rom-library`
- Helper: `/mnt/data/rom-library/bin/romlib_helper.py`
- Launcher: `/mnt/mmc/Roms/APPS/ROM Library.sh`
- Artwork: `/mnt/mmc/Roms/APPS/Imgs/ROM Library.png`
- Settings/logs: `/mnt/data/rom-library`
- Temporary or retained downloads: `/mnt/mmc/.rom-library-downloads`
- Recoverable trash: `/mnt/mmc/.rom-library-trash`
- Games: existing stock folders below `/mnt/mmc/Roms`

## Provider behavior and compatibility

The client uses the provider's compact JSON search endpoint, public game pages, documented page download POST, and returned static download host. It intentionally does not reproduce browser advertising, analytics, cookies, or unrelated requests. Provider-site format changes can temporarily break search or ticket acquisition; the app reports that condition without bypassing access controls.

The v1.1.0 release targets the RG35XX Pro stock aarch64 firmware identified by `RG35xxPRO`. It relies on firmware-provided SDL2, FreeType, JSON-GLib, libcurl, and Python 3, and includes the official static ARM64 build of 7-Zip 26.02 with its upstream license. Disc-based games may still need BIOS files; ROM Library does not install or alter BIOS content.

Version 1.1.0 identifies package content from its bytes instead of trusting the provider's filename. This specifically fixes PSP downloads such as a file named `.zip` whose provider metadata and bytes are actually 7z, then installs the contained ISO/CSO/CHD/PBP/PRX safely. It also preserves relative paths for cue sheets, playlists, Dreamcast GDI sets, and other multi-file media.

The stock firmware does not expose a PlayStation 2 emulator or `/mnt/mmc/Roms/PS2` folder. PS2 catalog entries remain unavailable instead of being installed into an unrelated system. See [FORMATS.md](docs/FORMATS.md) for the full firmware-derived matrix and intentional safety boundaries.

## Uninstall

From the extracted release directory, run `sh uninstall.sh`. This removes only the app executable, helper, launcher, and artwork. Games, settings, logs, retained downloads, and recoverable trash are preserved.
