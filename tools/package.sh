#!/bin/bash
# Builds the release ZIP (build/BL2VR.zip) from the current Release build.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$R/build/package/BL2VR"
rm -rf "$R/build/package" "$R/build/BL2VR.zip"
mkdir -p "$OUT"
cp "$R/build/Release/d3d9.dll" "$R/third_party/build-openxr/src/loader/Release/openxr_loader.dll" "$OUT/"
cp "$R/dist/bl2vr.ini" "$R/dist/LEEME.md" "$R/dist/instalar.bat" "$R/dist/instalar.ps1" "$R/LICENSE" "$OUT/"
# Notices for the bundled third-party code (MinHook is linked into d3d9.dll; the OpenXR loader
# ships as openxr_loader.dll and embeds jsoncpp).
{
    printf 'Componentes de terceros incluidos en Better Borderlands 2 VR\r\n\r\n'
    printf '=== MinHook (enlazado en d3d9.dll) ===\r\n\r\n'
    cat "$R/third_party/minhook/LICENSE.txt"
    printf '\r\n\r\n=== OpenXR loader (openxr_loader.dll), Apache License 2.0 ===\r\n\r\n'
    cat "$R/third_party/OpenXR-SDK/LICENSES/Apache-2.0.txt"
    printf '\r\n\r\n=== jsoncpp (dentro de openxr_loader.dll) ===\r\n\r\n'
    cat "$R/third_party/OpenXR-SDK/LICENSES/LicenseRef-jsoncpp-public-domain.txt"
} > "$OUT/TERCEROS.txt"
(cd "$R/build/package" && powershell -NoProfile -Command "Compress-Archive -Path BL2VR -DestinationPath ../BL2VR.zip -Force")
ls -la "$R/build/BL2VR.zip"
