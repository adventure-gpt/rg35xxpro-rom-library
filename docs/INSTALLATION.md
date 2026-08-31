# Easy installation guide for RG35XX Pro stock firmware

This guide assumes no previous SSH or Linux experience. It installs ROM Library 1.1.0 over a trusted home Wi-Fi network without removing the microSD card.

## What you need

- An Anbernic RG35XX Pro running stock firmware
- A Windows 10/11 computer or a Mac running a supported version of macOS
- The handheld and computer connected to the same non-guest Wi-Fi network
- About 10 minutes and at least 10 MB of free space

Do not enable SSH on public Wi-Fi. SSH provides administrator access to the handheld while the service is running.

## 1. Connect the handheld to Wi-Fi

1. Turn on the RG35XX Pro.
2. Open **Settings > Network Settings > WiFi**.
3. Turn Wi-Fi on, select the same network used by the computer, and enter its password.
4. Wait until the handheld reports that it is connected.

Guest Wi-Fi often prevents devices from communicating with each other. Use a normal trusted home network.

## 2. Enable the temporary SSH server

1. Open **App Center > APPS**.
2. Run **Temporary SSH Server**. Some stock-firmware versions place it inside **Modify System Tools**.
3. Press **A** to run it and wait about 10 seconds.
4. If **Settings > Network Settings** contains **Restart SSH Service**, run that once too.

The utility may return to the menu without a detailed success screen. That is normal.

## 3. Find the IP address

Return to **Settings > Network Settings** and write down the handheld's IP address. It will resemble `192.168.1.84` or `10.0.0.42`.

If the address is not shown there, start RetroArch and open **Information > Network Information**. Use the address beside `wlan0`.

## 4. Open a terminal on the computer

### Windows

1. Open the Windows **Start** menu.
2. Type `PowerShell`.
3. Open **Windows PowerShell** or **Terminal**. Administrator mode is not required.

If Windows later says that `ssh` is not recognized, open Windows **Optional Features**, install **OpenSSH Client**, and reopen PowerShell.

### macOS

1. Press **Command (⌘) + Space** to open Spotlight.
2. Type `Terminal`.
3. Press Return to open **Terminal**. You do not need to use `sudo`.

macOS already includes the `ssh` and `scp` commands used by this guide. No extra software is required.

## 5. Connect to the handheld

In PowerShell on Windows or Terminal on macOS, replace `YOUR_IP_ADDRESS` with the address from Step 3, then press Enter:

```sh
ssh root@YOUR_IP_ADDRESS
```

For example:

```sh
ssh root@192.168.1.84
```

On the first connection, type `yes` and press Enter when asked whether to continue connecting. When asked for a password, type `root` and press Enter. SSH deliberately shows no dots or stars while a password is typed.

A successful connection ends at a prompt similar to:

```text
root@ANBERNIC:~#
```

## 6. Download, verify, and install

Copy the complete block below, paste it into the connected PowerShell or Terminal window, and press Enter. The `&&` operators prevent later steps from running if a download or verification step fails.

```sh
cd /tmp && \
wget -O RG35XXPro-ROM-Library-v1.1.0.tar.gz 'https://github.com/adventure-gpt/rg35xxpro-rom-library/releases/download/v1.1.0/RG35XXPro-ROM-Library-v1.1.0.tar.gz' && \
wget -O RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256 'https://github.com/adventure-gpt/rg35xxpro-rom-library/releases/download/v1.1.0/RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256' && \
sha256sum -c RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256 && \
tar -xzf RG35XXPro-ROM-Library-v1.1.0.tar.gz && \
cd RG35XXPro-ROM-Library-v1.1.0 && \
sh install.sh
```

Do not continue if checksum verification does not report:

```text
RG35XXPro-ROM-Library-v1.1.0.tar.gz: OK
```

A successful installation ends with:

```text
ROM Library 1.1.0 installed successfully.
```

The published archive's SHA-256 is:

```text
49a1138a6e80a82ffdccaf676e886567a8431d94e9c9111f81bd3fffe47e4c6b
```

## 7. Verify and disconnect

Run:

```sh
cat /mnt/data/rom-library/VERSION
```

It must print `1.1.0`. Then run:

```sh
exit
```

Reboot the handheld to end a temporary SSH session. If the firmware provides an explicit Stop or Disable SSH option, use it.

## 8. Launch ROM Library

1. Leave the handheld's APPS list and open it again. Reboot once if the new entry does not appear.
2. Open **App Center > APPS > ROM Library**.
3. Use the D-pad to navigate, **A** to select, **B** to go back, and the round **Menu** button to exit.
4. Keep Wi-Fi enabled while searching or downloading.

## Troubleshooting

### Connection timed out or was refused

Confirm that both devices use the same non-guest Wi-Fi. Recheck the IP address, run **Temporary SSH Server** again, and use **Restart SSH Service** if available.

### The password appears frozen

This is normal. Type `root` even though nothing appears, then press Enter.

### Remote host identification changed

If the handheld's system card was recently reflashed, or the IP address now belongs to this handheld, run the following on the computer and reconnect:

```sh
ssh-keygen -R YOUR_IP_ADDRESS
```

### The handheld cannot download from GitHub

Download both release files into the computer's Downloads folder:

- `RG35XXPro-ROM-Library-v1.1.0.tar.gz`
- `RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256`

Then replace the IP address and run the matching command in a separate window.

On Windows PowerShell:

```powershell
scp "$HOME\Downloads\RG35XXPro-ROM-Library-v1.1.0.tar.gz" "$HOME\Downloads\RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256" root@YOUR_IP_ADDRESS:/tmp/
```

On macOS Terminal:

```sh
scp "$HOME/Downloads/RG35XXPro-ROM-Library-v1.1.0.tar.gz" "$HOME/Downloads/RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256" root@YOUR_IP_ADDRESS:/tmp/
```

Reconnect over SSH, begin Step 6 at the `sha256sum` command, and continue only if verification reports `OK`.

### Checksum verification failed

The archive is incomplete or damaged. Remove only the two temporary release files and repeat Step 6:

```sh
rm -f /tmp/RG35XXPro-ROM-Library-v1.1.0.tar.gz /tmp/RG35XXPro-ROM-Library-v1.1.0.tar.gz.sha256
```

### Firmware compatibility check failed

Do not bypass it. This release supports only RG35XX Pro stock firmware identified by the board name `RG35xxPRO`.

## Updating later

Repeat this guide with the newer release's filenames and checksum. The installer backs up the prior app files and preserves games, settings, downloads, and recoverable trash.

## Uninstalling

From an extracted release directory, run:

```sh
sh uninstall.sh
```

Uninstallation removes only the application, helper, launcher, and artwork. Games, settings, downloads, logs, and recoverable trash remain untouched.
