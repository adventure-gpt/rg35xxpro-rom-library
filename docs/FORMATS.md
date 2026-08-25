# RG35XX Pro format coverage

ROM Library 1.1.0 derives its destination folders and launchable extensions from the stock RG35XX Pro launcher's own file filters. A provider filename is only a hint: the installer identifies common archive and disc-image signatures from the downloaded bytes.

## Package containers

The installer handles ZIP/ZIP64/ZIPX, 7z, RAR/RAR5, tar, gzip/tar.gz, bzip2/tar.bz2, xz/tar.xz, Zstandard/tar.zst, LHA/LZH, ARJ, CAB, and CPIO. These containers may be nested up to four levels. Split 7z, ZIP, or RAR sets work when every volume is present together inside the downloaded outer package.

Direct game files, archive-wrapped game files, nested archives, and multi-file disc sets are supported. Arcade/DOS/ONS/ScummVM ZIPs are retained as launchable ROM sets; non-ZIP wrappers around raw arcade sets are safely repacked to ZIP. AMIGA LHA and Saturn RAR remain intact because the stock launcher accepts those forms directly.

Encrypted packages are rejected because the on-device interface does not collect passwords. The installer also rejects traversal, links, special files, unsafe FAT/exFAT names, case-insensitive collisions, more than 8,192 entries, more than 16 GiB expanded data, more than four nested archive layers, and extreme compression ratios.

## Stock system matrix

| ROM folder | Accepted game and descriptor extensions |
| --- | --- |
| A2600 | a26, bin, zip |
| A5200 | a52, zip |
| A7800 | a78, bin, zip |
| A800 | atr, rom, zip |
| AMIGA | adf, adz, chd, cue, dms, hdf, hdz, ipf, iso, lha, m3u, uae, zip |
| ATARIST | dim, ipf, m3u, msa, st, stx, zip |
| ATOMISWAVE | 7z, bin, chd, cue, dat, gdi, iso, lst, zip |
| C64 | bin, cmd, crt, d64/d71/d80/d81/d82, compressed disk variants, g41/g64, m3u, nib/nbz, p00, prg, t64, tap, vsf, x64, zip |
| CPS1 / CPS2 / CPS3 | zip |
| DOS | dosz, zip, com, bat, exe |
| DREAMCAST | bin, cdi, chd, cue, gdi, iso, m3u, zip plus referenced raw tracks |
| EASYRPG | ldb, zip |
| FBNEO | zip |
| FC | nes, unf, unif, zip |
| FDS | fds, zip |
| GB / GBC / GBA | gb / gbc / gba, zip |
| GG | gg, zip |
| GW | mgw |
| HBMAME / MAME | zip and CHD media |
| JAVA | jar |
| LYNX | lnx, zip |
| MD | bin, gen, md, smd, zip |
| MDCD | chd, cue, iso, m3u, sg, zip plus referenced tracks |
| MSX | cas, col, dsk, m3u, mx1, mx2, ri, rom, sc, sg, zip |
| N64 | bin, n64, rom, v64, z64, zip |
| NAOMI | 7z, bin, chd, cue, dat, gdi, iso, zip |
| NDS | nds, zip |
| NEOCD | chd, cue, iso, zip plus referenced tracks |
| NEOGEO | zip |
| NGP | ngc, ngp, zip |
| ONS | dat, nt/nt2/nt3, ons, txt, zip |
| OPENBOR | pak |
| PCE | bin, ccd, chd, cue, img, iso, pce, zip |
| PCECD | ccd, chd, cue, m3u, toc plus referenced tracks |
| PGM2 | zip |
| PICO | p8, PNG cartridges |
| POKE | min, zip |
| PS | bin/cue, cbn, ccd/img/sub, chd, iso, m3u, mds/mdf, pbp, toc and SBI sidecars |
| PSP | chd, cso, iso, pbp, prx |
| SATURN | bin/cue, ccd/img/sub, chd, iso, mds/mdf, rar |
| SCUMMVM | scummvm, zip |
| SEGA32X | 32x, bin, md, smd, zip |
| SFC | fig, sfc, smc, zip |
| SMS | bin, sms, zip |
| VARCADE | zip |
| VB | vb, vboy, zip |
| VIC20 | a0/20/b0, bin, cmd, crt, d6/d7/d8, g4/g6, gz, m3u, nib/nbz, p00, prg, t64, tap, vsf, x6, zip |
| WS | ws, wsc, zip |

Track and descriptor sidecars are installed together with the playable entry and preserved in their relative directory layout. Unsupported documentation, advertisements, executables outside the stock DOS/JAVA formats, and unrelated files are ignored.

## Not a supported emulator

The stock RG35XX Pro firmware advertises PSP, PS1, Dreamcast, NDS, N64, and older systems, but not PS2. It has no PS2 launcher entry or `PS2` ROM folder. ROM Library therefore leaves PS2 provider results unavailable rather than pretending they can run.
