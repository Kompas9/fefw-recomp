# Fetch the pinned third-party sources into tools\textures\third_party (git-ignored). Versions: THIRD_PARTY.md.
$ErrorActionPreference = 'Stop'
$tp = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\third_party'))
New-Item -ItemType Directory -Force $tp | Out-Null
if (-not (Test-Path "$tp\astc-encoder")) {
    git clone https://github.com/ARM-software/astc-encoder.git "$tp\astc-encoder"
    git -C "$tp\astc-encoder" checkout 5.7.1
}
if (-not (Test-Path "$tp\bc7enc_rdo")) {
    git clone https://github.com/richgel999/bc7enc_rdo.git "$tp\bc7enc_rdo"
    git -C "$tp\bc7enc_rdo" checkout b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
}
if (-not (Test-Path "$tp\ispc\ispc-v1.32.0-windows\bin\ispc.exe")) {
    $zip = Join-Path $env:TEMP 'ispc-v1.32.0-windows.zip'
    curl.exe -L -o $zip https://github.com/ispc/ispc/releases/download/v1.32.0/ispc-v1.32.0-windows.zip
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($h -ne '148D5CD96ABE7501C730D5FBD00F1F0C893B96BFEADA38D1811C9B6251E86A5C') { throw "ispc zip hash mismatch: $h" }
    Expand-Archive $zip "$tp\ispc"
}
Write-Output "third_party ready in $tp"
