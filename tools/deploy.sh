#!/bin/bash
# Stops BL2, installs the freshly built mod into the game folder.
G="W:/SteamLibrary/steamapps/common/Borderlands 2/Binaries/Win32"
R="$(dirname "$0")/.."
powershell -NoProfile -Command "Stop-Process -Name Borderlands2 -Force -ErrorAction SilentlyContinue; Start-Sleep 2"
cp "$R/build/Release/d3d9.dll" "$R/third_party/build-openxr/src/loader/Release/openxr_loader.dll" "$G/" && echo deployed
