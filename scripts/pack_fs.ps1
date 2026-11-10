# Pack Relapse-Exploit web assets into a LittleFS image (storage partition @ 0x110000, 2MB).
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\pack_fs.ps1 -Repo "E:\Relapse-Exploit-main"
param(
    [string]$Repo = "E:\Relapse-Exploit-main",
    [string]$Mklittlefs = "$PSScriptRoot\..\tools\mklittlefs.exe",
    [int]$SizeKB = 2048
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."

if (-not (Test-Path $Repo)) { throw "Repo dir not found: $Repo" }
if (-not (Test-Path $Mklittlefs)) {
    throw "mklittlefs.exe not found at $Mklittlefs - download the win32 build from https://github.com/earlephilhower/esp-quick-toolchain/releases and put it into tools\ (or pass -Mklittlefs)"
}

# 1. Stage the files that serve.py would expose (same layout as the repo)
$data = Join-Path $root "build_data"
if (Test-Path $data) { Remove-Item -Recurse -Force $data }
New-Item -ItemType Directory -Path $data | Out-Null

$items = @("index.html", "src", "offsets", "payloads", "LICENSE", "README.md")
foreach ($item in $items) {
    $src = Join-Path $Repo $item
    if (-not (Test-Path $src)) { throw "Missing $item in repo, check -Repo path" }
    Copy-Item -Recurse -Force $src (Join-Path $data $item)
}

$total = (Get-ChildItem -Recurse -File $data | Measure-Object Length -Sum).Sum
"{0:N0} bytes staged ({1:N1} MB)" -f $total, ($total / 1MB)

if ($total -gt $SizeKB * 1024) { throw "Assets exceed partition size ${SizeKB}KB" }

# 2. Build the LittleFS image
$img = Join-Path $root "littlefs.bin"
& $Mklittlefs -c $data -p 256 -b 4096 -s ($SizeKB * 1024) $img
if ($LASTEXITCODE -ne 0) { throw "mklittlefs failed" }

"Image written: $img"
"Flash with:    python -m esptool --chip esp32s3 -p <COM> -b 921600 write_flash 0x110000 littlefs.bin"
