# PS5 Relapse 随身越狱棒

[English](README.en.md)

把一块 **ESP32-C3 / ESP32-S3** 开发板（4MB Flash 以上）变成一个便携 WiFi 热点，
开机即可给 PS5 浏览器分发 [Relapse-Exploit](https://github.com/ntfargo/Relapse-Exploit) 网页利用链。
支持 ESP32-C3（SuperMini / DevKitM-1 等）与 ESP32-S3，**同一份源码，无功能裁剪**。

```
PS5 浏览器 ──WiFi──> ESP32 (AP: 192.168.4.1)
                      ├─ HTTP :8000 / (exploit 入口)
                      ├─ HTTP :8000/manage (payload 管理页)
                      └─ TCP 9021 推送 (elfldr)
```

本仓库同时包含 Relapse-Exploit 的网页源码（根目录的 `index.html / src / offsets / payloads / serve.py`），
可以脱离固件直接用 Python 跑 `serve.py` 在 PC 上分发 exploit。

## 目录结构

### ESP32 固件

| 路径 | 说明 |
|---|---|
| `main/app_main.cpp` | 固件主程序（WiFi AP + HTTP 服务器 + 管理页） |
| `main/Kconfig.projbuild` | menuconfig 中的热点 SSID / 密码配置 |
| `partitions.csv` | 分区表：NVS / phy_init / factory(1MB) / storage-LittleFS(2MB) |
| `sdkconfig.defaults` | 默认配置（Flash=4MB, LWIP sockets=16, HTTPD 头=1KB） |
| `scripts/pack_fs.ps1` | 把 exploit 网页 + payload 打入 LittleFS 镜像 |
| `tools/mklittlefs.exe` | Windows LittleFS 镜像生成工具（已内置） |

### Relapse-Exploit 网页源码（fork 自上游）

| 路径 | 说明 |
|---|---|
| `index.html` | exploit 入口页 |
| `src/` | JS 利用链（webkit / kexp / firmware / rop / site / utils） |
| `offsets/` | 各 PS5 固件版本的偏移表（7.00 – 13.60） |
| `payloads/` | elfldr + kexp 内核利用 |
| `serve.py` | 纯 Python 分发脚本（无需固件，PC 直接跑） |

## 硬件要求

- **ESP32-C3**（推荐）：SuperMini / DevKitM-1 / Air780E 等，**Flash ≥ 4MB**（无 PSRAM 也可）
  - C3 有两种接法：原生 USB（USB-CDC）或外部 UART 桥（常见 SuperMini）
- **ESP32-S3**：任意 4MB+ Flash 模块，带 PSRAM 则并行资源下载更稳
- 一根能传数据的 USB 线
- PS5（支持 Relapse 的固件版本 7.00 – 13.60）

分区表合计约 **3.1MB**（bootloader 0.2MB + 分区表 0.01MB + app 1MB + storage 2MB），
4MB Flash 的模块放得下。更大的 8/16MB Flash 直接兼容，分区表不需要改。

## 两种用法

### A. ESP32 随身棒（推荐）

最便携，刷一次固件以后每次开机就是越狱分发主机。固件同时 serve exploit 网页 + 提供 `/manage` 管理页。

#### 快速上手（直接烧录 Release 固件）

不想从源码编译？下载 [GitHub Releases](../../releases) 里的预编译包，用 esptool 一次性烧入：

**C3 板（SuperMini / DevKitM-1）**

```powershell
python -m esptool --chip esp32c3 -p COM5 -b 460800 --before default_reset --after hard_reset `
  write_flash 0x0     bootloader.bin `
              0x8000  partition-table.bin `
              0x10000 ps5_relapse_stick_c3.bin `
              0x110000 littlefs.bin
```

**S3 板**

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 --before default_reset --after hard_reset `
  write_flash 0x0     bootloader.bin `
              0x8000  partition-table.bin `
              0x10000 ps5_relapse_stick_s3.bin `
              0x110000 littlefs.bin
```

> **原生 USB-CDC C3 板**若 `--before default_reset` 进不了下载模式：按住 BOOT 键上电，
> 然后改用 `--before no_reset`。外部 UART 桥的 SuperMini 直接烧即可。

#### 从源码编译固件

**1. 环境**

- [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/get-started/)（Windows 推荐用 ESP-IDF 官方安装器）
- 首次构建需要联网（`joltwallet/littlefs` 组件由 ESP-IDF 组件管理器自动拉取）

**2. 把 exploit 网页 + payload 打入 LittleFS**

仓库根目录已经有 Relapse-Exploit 的原始网页文件。用 `pack_fs.ps1` 把它们打包进 LittleFS 镜像：

```powershell
# 用本仓库根目录现成的 exploit 资源
powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "."

# 或者你拉了 Relapse-Exploit 上游最新版：
# powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "E:\Relapse-Exploit-main"
```

生成 `littlefs.bin`（2MB），后面需要烧到 `0x110000`。

**3. 编译**

```powershell
# C3 板
idf.py set-target esp32c3
# 或 S3 板
idf.py set-target esp32s3

# 可选：修改热点 SSID / 密码（默认 PS5-RELAPSE / 12345678）
idf.py menuconfig → PS5 Relapse Stick

idf.py build
```

**4. 烧写**

```powershell
# 一键烧写固件 + 资源分区
idf.py -p COM5 flash
```

#### 烧录地址速查

| 地址 | 文件 | 内容 |
|---|---|---|
| `0x0` | `build\bootloader\bootloader.bin` | 引导程序 |
| `0x8000` | `build\partition_table\partition-table.bin` | 分区表 |
| `0x10000` | `build\ps5_relapse_stick_c3.bin` / `_s3.bin` | 应用固件（1MB） |
| `0x110000` | `littlefs.bin` | exploit 资源（2MB LittleFS） |

- 只改了应用代码（重编译后）：只烧 `0x10000` 地址的 app bin
- 只更新了 exploit 资源（重跑 pack_fs.ps1 后）：只烧 `0x110000 littlefs.bin`
- 换 target（C3 ↔ S3）必须整组重烧（bootloader / app 指令集不同）

### B. PC 直接跑 serve.py

适合调试 exploit 或临时分发：

```bash
python serve.py
# 浏览器打开 http://<PC-IP>:8000/
```

PC 和 PS5 需要在同一局域网（或用手机热点桥接）。

## 使用步骤（ESP32 棒）

1. 上电，等待热点起来（约 2 秒）。日志会打印：
   ```
   AP up: SSID="PS5-RELAPSE" channel=1
   ```
2. PS5：**设定 → 网络 → 自定义 → WiFi**，连接 `PS5-RELAPSE`（密码 `12345678`）
3. PS5 浏览器访问 **`http://192.168.4.1:8000/`**
4. 页面会自动识别 PS5 固件版本（UA `PlayStation 5/x.xx`）并运行 Relapse 利用链
   - WebKit 利用阶段可能需要多次刷新（属正常现象）
   - 内核阶段如果 panic，PS5 会直接重启，重开浏览器即可
5. 越狱成功后 elfldr 在 PS5 上监听 `:9021`，这时：
   - PS5 IP 会被 ESP32 **自动记录**（谁打开了 exploit 入口页就记谁）
   - 手机 / 电脑（同一热点）访问 **`http://192.168.4.1:8000/manage`** 进入管理页

## 管理页 /manage

嵌入在固件中的轻量管理界面（**不在 LittleFS 里**，重打包 exploit 资源不影响）：

- **上传自定义 payload**：选 ELF / bin 文件（单文件 ≤ 1MB）→ POST 到设备，存 `payloads_custom/`
- **列表 / 删除**：显示文件名、大小、分区剩余空间，5 秒自动刷新
- **发送到 PS5**：ESP32 作为 TCP 客户端把文件推给 `PS5_IP:9021`（elfldr），**不需要 PC 上的 nc**
- **PS5 IP 自动记录**：打开过 exploit 入口页的设备即被识别，也可手动覆盖

典型流程：
```
越狱成功 → 管理页上传 payload.elf → 点"发送到 PS5" → 完成
```

> ⚠️ 注意：重跑 `pack_fs.ps1` 并烧写 `0x110000` 会**覆盖整个 storage 分区**，
> 已上传的自定义 payload 会丢失。建议先用 `scp` / `ps5-pkg` 工具把 payload 拉到 PC 再更新。
> 管理页**没有鉴权**（个人热点场景），不要把设备暴露到公共网络。

## FAQ / 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| 页面空白、控制台无输出 | `.js` 的 MIME 必须是 `application/javascript`（本固件已处理）；确认访问的是 `:8000` |
| 报 `FW x.xx is not supported` | exploit 不支持该固件版本；或缓存了旧的 `offsets/`，硬刷新 PS5 浏览器（本固件已发 `no-store`） |
| WebKit 阶段反复失败 | 属正常，Relapse 需要多次尝试；确保 PS5 信号满格 |
| 大文件 `.elf` 下载失败 | 靠热点太近，或 LWIP socket 不够；固件已配 `CONFIG_LWIP_MAX_SOCKETS=16` |
| 首次启动日志 `0/0 bytes used` | 忘记烧 `littlefs.bin`，烧写 `0x110000` 地址 |
| 串口日志 `httpd start failed` | `max_open_sockets` 不够或 PSRAM 初始化失败；S3 板建议启用 PSRAM |
| C3 SuperMini 烧不动 | 按住 BOOT 键上电，改用 `--before no_reset` |

## 自定义热点 SSID / 密码

```powershell
idf.py menuconfig → PS5 Relapse Stick
```

默认值：SSID `PS5-RELAPSE` / 密码 `12345678`。密码需 ≥ 8 位（WPA2 要求）。

## 更新 exploit 资源

Relapse-Exploit 上游有新的网页 / payload 时：

```powershell
# 拉上游最新版到 E:\Relapse-Exploit-main
powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "E:\Relapse-Exploit-main"
python -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash 0x110000 littlefs.bin
```

**无需重新编译固件**（固件只是 HTTP 服务器，资源全在 LittleFS 里）。

## Exploit chain（Relapse 原生）

Browser stage uses JSC info leaks and a structured clone object pool mismatch to corrupt a typedarray. The kernel stage combines an address leak with an `aio_multi_wait` uaf race to establish kernel r/w.

Webkit may need several attempts — reload the page if the browser stalls. The kernel exploit may hang or panic the console, so reboot before trying again if that happens.

## Credits

Exploit credits（来自上游 Relapse-Exploit 仓库）：

- **Sonic_Iso**: Kernel Exploit
- **Jordy**: Webkit Exploit and Kernel Bug
- **ntfargo**: Exploit Dev
- **ufm42**: Exploit Dev
- **Dr. Yenyen**: Testing
- TheFlow, SlidyBat, Flatz, cow, nhk, bollarz, Sleirsgoevy, EchoStretch, EarthOnion

ESP32 固件移植 & `/manage` 管理页：silentfox2023

## 合规提示

本工程仅用于**教育与安全研究**：请只在你拥有或获授权的 PS5 上使用。
MIT 许可证要求保留上游 Relapse-Exploit 的 `LICENSE`，打包脚本已将其一并放入 LittleFS 文件系统。

The software is provided as-is, without warranty. You assume the risks of using it, including system instability, data loss, and account bans. The maintainers accept no liability for resulting damage.
