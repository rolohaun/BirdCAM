param(
  [Parameter(Mandatory = $true)]
  [string] $Version,

  [string] $Repo = "rolohaun/BirdCAM"
)

$ErrorActionPreference = "Stop"

if ($Version.StartsWith("v")) {
  $Version = $Version.Substring(1)
}

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$mainPath = Join-Path $root "src/main.cpp"
$manifestPath = Join-Path $root "firmware/manifest.json"
$distDir = Join-Path $root "dist"
$binName = "birdcam-$Version.bin"
$distBin = Join-Path $distDir $binName
$releaseTag = "v$Version"
$downloadUrl = "https://github.com/$Repo/releases/download/$releaseTag/$binName"

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$pioCommand = Get-Command platformio -ErrorAction SilentlyContinue
if ($null -eq $pioCommand) {
  $pioPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
  if (!(Test-Path $pioPath)) {
    throw "PlatformIO was not found on PATH or at $pioPath"
  }
} else {
  $pioPath = $pioCommand.Source
}

$source = Get-Content -Raw -Path $mainPath
$source = $source -replace 'static const char \*FIRMWARE_VERSION = "[^"]+";', "static const char *FIRMWARE_VERSION = `"$Version`";"
Set-Content -Path $mainPath -Value $source -NoNewline

& $pioPath run
Copy-Item -Force -Path (Join-Path $root ".pio/build/esp32cam/firmware.bin") -Destination $distBin

$sha256 = (Get-FileHash -Algorithm SHA256 -Path $distBin).Hash.ToLowerInvariant()
$manifest = [ordered]@{
  version = $Version
  url = $downloadUrl
  sha256 = $sha256
}

($manifest | ConvertTo-Json -Depth 3) + "`n" | Set-Content -Path $manifestPath -NoNewline

git add src/main.cpp firmware/manifest.json platformio.ini README.md .gitignore src/secrets.example.h scripts/publish-ota.ps1
git commit -m "Release firmware $Version"
git push

gh release view $releaseTag --repo $Repo *> $null
if ($LASTEXITCODE -eq 0) {
  gh release upload $releaseTag $distBin --repo $Repo --clobber
} else {
  gh release create $releaseTag $distBin --repo $Repo --title "BirdCAM $Version" --notes "BirdCAM firmware $Version"
}

Write-Host "Published OTA firmware $Version"
Write-Host "Manifest: $manifestPath"
Write-Host "SHA-256: $sha256"
