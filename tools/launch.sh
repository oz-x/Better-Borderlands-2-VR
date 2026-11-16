#!/bin/bash
# Starts BL2 directly (Steam must be running); skips the 2K launcher.
G="W:/SteamLibrary/steamapps/common/Borderlands 2/Binaries/Win32"
powershell -NoProfile -Command "\$env:SteamAppId='49520'; \$env:SteamGameId='49520'; if ('$XR_RUNTIME_JSON') { \$env:XR_RUNTIME_JSON='$XR_RUNTIME_JSON' }; Start-Process -FilePath '$G/Borderlands2.exe' -WorkingDirectory '$G'"
for i in $(seq 1 20); do
  sleep 2
  tasklist | grep -q -i Borderlands2.exe && { echo "game started"; exit 0; }
done
echo "game did not start"; exit 1
