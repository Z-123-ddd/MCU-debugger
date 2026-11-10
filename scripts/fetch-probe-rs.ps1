# Fetch the official probe-rs binaries into tools/probe-rs/
#
# Why a script: the app uses the official `probe-rs` CLI as its debug backend
# (see README). The binaries are NOT redistributed with this repository.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-probe-rs.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-probe-rs.ps1 -Version v0.32.0 -Force
#
# Layout after a successful run:
#   tools\probe-rs\probe-rs.exe
#   tools\probe-rs\VERSION.txt

param(
    [string]$Version = "v0.32.0",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root "tools\probe-rs"
$exe = Join-Path $target "probe-rs.exe"

if ((Test-Path $exe) -and -not $Force) {
    Write-Host "[INFO] 已存在: $exe (使用 -Force 重新下载)"
    & $exe --version
    exit 0
}

New-Item -ItemType Directory -Force -Path $target | Out-Null

$asset = "probe-rs-tools-x86_64-pc-windows-msvc.zip"
$baseUrl = "https://github.com/probe-rs/probe-rs/releases/download/$Version"
$zipUrl = "$baseUrl/$asset"
$shaUrl = "$zipUrl.sha256"
$zipPath = Join-Path $env:TEMP $asset

Write-Host "[INFO] 下载 $zipUrl"
try {
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing -TimeoutSec 600
} catch {
    Write-Host "[WARN] Invoke-WebRequest 失败: $($_.Exception.Message)"
    Write-Host "[INFO] 尝试使用 curl.exe ..."
    & curl.exe -L --fail --connect-timeout 20 -o $zipPath $zipUrl
    if ($LASTEXITCODE -ne 0) { throw "下载失败，请手动下载 $zipUrl 并解压到 $target" }
}

# Optional checksum verification (best effort: skip when unreachable).
try {
    $expected = (Invoke-WebRequest -Uri $shaUrl -UseBasicParsing -TimeoutSec 60).Content.Trim().Split(" ")[0]
    $actual = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToLower()
    if ($expected -and ($expected.ToLower() -ne $actual)) {
        throw "SHA256 校验失败: expected=$expected actual=$actual"
    }
    Write-Host "[OK] SHA256 校验通过"
} catch {
    Write-Host "[WARN] 跳过 SHA256 校验: $($_.Exception.Message)"
}

Write-Host "[INFO] 解压到 $target"
Expand-Archive -Path $zipPath -DestinationPath $target -Force
Remove-Item $zipPath -Force -ErrorAction SilentlyContinue

if (-not (Test-Path $exe)) {
    $found = Get-ChildItem -Path $target -Filter "probe-rs.exe" -Recurse -File | Select-Object -First 1
    if ($null -eq $found) { throw "解压后未找到 probe-rs.exe" }
    if ($found.Directory.FullName -ne $target) {
        Move-Item -Path $found.FullName -Destination $exe -Force
    }
}

Set-Content -Path (Join-Path $target "VERSION.txt") -Value $Version -Encoding ASCII
Write-Host "[OK] $exe"
& $exe --version
