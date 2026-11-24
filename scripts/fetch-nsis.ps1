# ---------------------------------------------------------------------------
# Fetch the NSIS toolchain used to build the installer.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-nsis.ps1
#
# Result: tools\nsis\makensis.exe
#
# Why the portable zip instead of the NSIS installer: no administrator rights are
# needed, nothing is added to the user's machine, and `tools\` is already the
# convention this repo uses for fetched toolchains (see .gitignore).
#
# Downloads go through curl.exe (bundled with Windows 10+) rather than
# Invoke-WebRequest: SourceForge answers IWR with a small HTML interstitial
# instead of the archive, while curl gets the real file.
# ---------------------------------------------------------------------------
param(
    [string]$Version = "3.11"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dest = Join-Path $root "tools\nsis"
$makensis = Join-Path $dest "makensis.exe"

if (Test-Path $makensis) {
    $reported = & $makensis /VERSION
    Write-Host "[OK] makensis already present (NSIS $reported): $makensis"
    exit 0
}

$curl = Join-Path $env:SystemRoot "System32\curl.exe"
if (-not (Test-Path $curl)) {
    throw "curl.exe not found; install NSIS manually and set NSIS_HOME"
}

$urls = @(
    "https://downloads.sourceforge.net/project/nsis/NSIS%203/$Version/nsis-$Version.zip",
    "https://master.dl.sourceforge.net/project/nsis/NSIS%203/$Version/nsis-$Version.zip?viasf=1"
)

$zip  = Join-Path $env:TEMP "nsis-$Version.zip"
$work = Join-Path $env:TEMP "nsis-$Version-extract"

$fetched = $false
foreach ($url in $urls) {
    Write-Host "[INFO] downloading NSIS $Version"
    Write-Host "       $url"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    & $curl -sSL --max-time 180 -o $zip $url 2>$null
    if (-not (Test-Path $zip)) { continue }

    # Validate before extracting: a small HTML page instead of the archive is
    # exactly the failure this check exists to catch.
    $length = (Get-Item $zip).Length
    $head = [System.IO.File]::ReadAllBytes($zip)
    if ($length -gt 1000000 -and $head[0] -eq 0x50 -and $head[1] -eq 0x4B) {
        Write-Host ("[INFO] downloaded {0:N1} MB" -f ($length / 1MB))
        $fetched = $true
        break
    }
    Write-Host "[WARN] not a zip archive ($length bytes), trying the next mirror"
}
if (-not $fetched) {
    throw "could not download NSIS $Version from any mirror"
}

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
Write-Host "[INFO] extracting"
Expand-Archive -Path $zip -DestinationPath $work -Force

$inner = Join-Path $work "nsis-$Version"
if (-not (Test-Path $inner)) {
    throw "unexpected archive layout: $inner not found"
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Path (Join-Path $inner "*") -Destination $dest -Recurse -Force

Remove-Item -Recurse -Force $work
Remove-Item -Force $zip

if (-not (Test-Path $makensis)) {
    throw "makensis.exe not found after extraction"
}

$reported = & $makensis /VERSION
Write-Host "[OK] NSIS $reported -> $makensis"
