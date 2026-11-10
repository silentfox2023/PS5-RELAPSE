# PS5 Relapse Portable Jailbreak Stick

Turn any **ESP32-C3 / ESP32-S3** dev board (4MB Flash or larger) into a pocket-sized WiFi hotspot
that serves the [Relapse-Exploit](https://github.com/ntfargo/Relapse-Exploit) web exploit chain to
a PS5 browser over the air. Same firmware source supports both chips — zero feature trimming.

```
PS5 browser ──WiFi──> ESP32 (AP: 192.168.4.1)
                      ├─ HTTP :8000 / (exploit landing page)
                      ├─ HTTP :8000/manage (payload management)
                      └─ TCP 9021 push (elfldr)
```

This repo also ships the upstream Relapse-Exploit web assets at the repo root
(`index.html / src / offsets / payloads / serve.py`), so you can run `serve.py` directly on a PC
without any ESP32 firmware.

[中文说明](README.md)

## Repository layout

### ESP32 firmware

| Path | Description |
|---|---|
| `main/app_main.cpp` | Firmware entry point (WiFi AP + HTTP server + management UI) |
| `main/Kconfig.projbuild` | SSID / password settings exposed via `menuconfig` |
| `partitions.csv` | Partition table: NVS / phy_init / factory(1MB) / storage-LittleFS(2MB) |
| `sdkconfig.defaults` | Default build options (Flash=4MB, LWIP sockets=16, HTTPD header=1KB) |
| `scripts/pack_fs.ps1` | Bundle exploit web + payloads into a LittleFS image |
| `tools/mklittlefs.exe` | Windows LittleFS image builder (included, no extra download needed) |

### Relapse-Exploit web assets (upstream fork)

| Path | Description |
|---|---|
| `index.html` | Exploit landing page |
| `src/` | JS exploit chain (webkit / kexp / firmware / rop / site / utils) |
| `offsets/` | PS5 firmware offsets (7.00 – 13.60) |
| `payloads/` | elfldr + kexp kernel exploit |
| `serve.py` | Minimal Python server — no firmware needed |

## Hardware

- **ESP32-C3** (recommended): SuperMini / DevKitM-1 / Air780E — **Flash ≥ 4MB** (no PSRAM required)
  - C3 has two flash paths: native USB (USB-CDC) or an external UART bridge (common on SuperMini clones)
- **ESP32-S3**: Any 4MB+ Flash module; PSRAM helps parallel resource downloads
- A data-capable USB cable (charge-only cables won't work)
- A PS5 running firmware **7.00 – 13.60** (Relapse support table lives in upstream repo)

The partition table totals roughly **3.1MB** (bootloader 0.2MB + table 0.01MB + app 1MB + storage 2MB),
which fits easily on a 4MB module. 8MB / 16MB Flash modules are drop-in compatible with no changes.

## Two ways to use it

### A. ESP32 stick (recommended)

Flash once, boot forever — the board acts as a dedicated exploit host every time you power it on.
The firmware serves both the exploit webpage and the `/manage` UI from a single 8000-port server.

#### Quick start — flash the prebuilt Release firmware

If you don't want to compile from source, grab the binaries from
[GitHub Releases](../../releases) and flash them in one shot:

**C3 board (SuperMini / DevKitM-1)**

```powershell
python -m esptool --chip esp32c3 -p COM5 -b 460800 --before default_reset --after hard_reset `
  write_flash 0x0     bootloader.bin `
              0x8000  partition-table.bin `
              0x10000 ps5_relapse_stick_c3.bin `
              0x110000 littlefs.bin
```

**S3 board**

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 --before default_reset --after hard_reset `
  write_flash 0x0     bootloader.bin `
              0x8000  partition-table.bin `
              0x10000 ps5_relapse_stick_s3.bin `
              0x110000 littlefs.bin
```

> **Native USB-CDC C3 boards** that refuse to enter download mode with `--before default_reset`:
> hold down BOOT while powering on, then re-run with `--before no_reset`. Boards with an external
> UART bridge (most SuperMinis) flash normally.

#### Building from source

**1. Toolchain**

- [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)
  (on Windows the official ESP-IDF Installer is the easiest path)
- First build needs internet — ESP-IDF's component manager pulls `joltwallet/littlefs` automatically

**2. Bundle the exploit web assets into LittleFS**

The repo root already contains upstream Relapse-Exploit files. Run:

```powershell
# Use the files already in this repo root
powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "."

# Or point it at a freshly cloned upstream:
# powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "E:\Relapse-Exploit-main"
```

This produces `littlefs.bin` (2MB) which must be flashed to `0x110000`.

**3. Compile**

```powershell
# C3 board
idf.py set-target esp32c3
# or S3 board
idf.py set-target esp32s3

# Optional: change hotspot SSID / password (defaults PS5-RELAPSE / 12345678)
idf.py menuconfig → PS5 Relapse Stick

idf.py build
```

**4. Flash**

```powershell
# One-command flash for both firmware + storage partition
idf.py -p COM5 flash
```

#### Flash address cheat-sheet

| Address | File | Content |
|---|---|---|
| `0x0` | `build\bootloader\bootloader.bin` | Bootloader |
| `0x8000` | `build\partition_table\partition-table.bin` | Partition table |
| `0x10000` | `build\ps5_relapse_stick_c3.bin` / `_s3.bin` | App firmware (1MB) |
| `0x110000` | `littlefs.bin` | Exploit assets (2MB LittleFS) |

- App code only (after rebuild): flash just the `0x10000` app bin
- Assets only (after re-running `pack_fs.ps1`): flash just `0x110000 littlefs.bin`
- Changing target (C3 ↔ S3): **must** reflash bootloader + app together (different instruction sets)

### B. Run `serve.py` directly on a PC

Handy for debugging the exploit or one-off sessions:

```bash
python serve.py
# browse http://<PC-IP>:8000/ from the PS5
```

PC and PS5 must be on the same LAN (or bridged via a phone hotspot).

## Using the ESP32 stick

1. Power it on. Wait ~2 seconds for the AP to come up — serial log prints:
   ```
   AP up: SSID="PS5-RELAPSE" channel=1
   ```
2. On the PS5: **Settings → Network → Custom → WiFi**, join `PS5-RELAPSE` (password `12345678`)
3. Open the PS5 browser and go to **`http://192.168.4.1:8000/`**
4. The page auto-detects the PS5 firmware version (UA `PlayStation 5/x.xx`) and runs the Relapse chain
   - The WebKit stage may need multiple reloads — this is normal per the upstream README
   - If the kernel stage panics the PS5 will reboot; just reopen the browser
5. Once elfldr is listening on port `:9021`:
   - The ESP32 **auto-records** the PS5 IP (whoever hit the exploit landing page)
   - Open **`http://192.168.4.1:8000/manage`** on any phone / PC on the same AP

## `/manage` page

A lightweight management UI baked into the firmware (**not** in LittleFS — repacking exploit assets never touches it):

- **Upload custom payloads**: pick an ELF / bin file (≤ 1 MB each) → POSTed to the device, stored in `payloads_custom/`
- **List / Delete**: shows filename, size, and remaining storage space; auto-refreshes every 5 seconds
- **Push to PS5**: the ESP32 opens a TCP connection and streams the file to `PS5_IP:9021` (elfldr) — **no `nc` on the PC side needed**
- **PS5 IP auto-record**: any device that opened the exploit landing page is remembered, or you can override it manually

Typical flow:
```
Jailbreak succeeds → /manage → upload payload.elf → "Push to PS5" → done
```

> ⚠️ Repacking exploit assets (`pack_fs.ps1` + flash `0x110000`) **overwrites the entire storage partition**,
> so previously uploaded custom payloads are lost. Pull them back with `scp` / `ps5-pkg` first.
> The manage page has **no auth** by design (personal hotspot scenario). Never expose the device
> to an untrusted network.

## FAQ / Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| Blank page, no console output | `.js` files must come with MIME `application/javascript` (handled by this firmware); confirm you're on port `:8000` |
| `FW x.xx is not supported` | Exploit doesn't cover that firmware; or stale `offsets/` cached — hard-refresh the PS5 browser (this firmware sends `no-store`) |
| WebKit stage keeps failing | Normal — Relapse often needs several attempts; make sure the PS5 has strong WiFi signal |
| Large `.elf` download fails | The ESP32 is too far from the PS5, or sockets exhausted; firmware defaults `CONFIG_LWIP_MAX_SOCKETS=16` |
| First boot log shows `0/0 bytes used` | You forgot to flash `littlefs.bin` — reflash `0x110000` |
| Serial log `httpd start failed` | `max_open_sockets` too low or PSRAM init failed; enable PSRAM on S3 boards |
| C3 SuperMini won't flash | Hold BOOT while powering on, then re-run with `--before no_reset` |

## Changing the hotspot SSID / password

```powershell
idf.py menuconfig → PS5 Relapse Stick
```

Defaults: SSID `PS5-RELAPSE`, password `12345678`. Password must be ≥ 8 characters (WPA2 requirement).

## Updating exploit assets

When upstream Relapse-Exploit ships new web pages or payloads:

```powershell
# pull the latest upstream into E:\Relapse-Exploit-main
powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "E:\Relapse-Exploit-main"
python -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash 0x110000 littlefs.bin
```

**No firmware rebuild needed** — the firmware is just a static HTTP server, all exploit assets live in LittleFS.

## Exploit chain (upstream Relapse)

Browser stage uses JSC info leaks and a structured clone object pool mismatch to corrupt a typedarray. The kernel stage combines an address leak with an `aio_multi_wait` uaf race to establish kernel r/w.

WebKit may need several attempts — reload the page if the browser stalls. The kernel exploit may hang or panic the console, so reboot before trying again if that happens.

## Credits

Exploit credits (from upstream Relapse-Exploit repo):

- **Sonic_Iso**: Kernel Exploit
- **Jordy**: Webkit Exploit and Kernel Bug
- **ntfargo**: Exploit Dev
- **ufm42**: Exploit Dev
- **Dr. Yenyen**: Testing
- TheFlow, SlidyBat, Flatz, cow, nhk, bollarz, Sleirsgoevy, EchoStretch, EarthOnion

ESP32 firmware port and `/manage` page: silentfox2023

## License / Disclaimer

This project is intended for **educational and security research purposes only**. Use it only on
devices you own or are authorized to test. Comply with all applicable laws in your jurisdiction.

The software is provided as-is, without warranty. You assume the risks of using it, including
system instability, data loss, and account bans. The maintainers accept no liability for
resulting damage.

MIT License — the upstream Relapse-Exploit `LICENSE` file is preserved and bundled inside the
LittleFS image by `pack_fs.ps1`.
