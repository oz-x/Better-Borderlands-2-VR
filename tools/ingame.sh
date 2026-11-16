#!/bin/bash
# Builds, deploys and launches BL2, skips the title screen and loads Windshear Waste.
R="$(cd "$(dirname "$0")/.." && pwd)"
G="W:/SteamLibrary/steamapps/common/Borderlands 2/Binaries/Win32"
cmake --build "$R/build" --config Release 2>&1 | grep -E " error |\->" | grep -v minhook
bash "$R/tools/deploy.sh" || exit 1
bash "$R/tools/launch.sh" || exit 1
for i in $(seq 1 60); do sleep 1; grep -q "Viewport client" "$G/bl2vr.log" 2>/dev/null && break; done
sleep 3
python "$R/tools/focuskey.py" RETURN
sleep 8
python "$R/tools/click.py" 0.285 0.69   # SHiFT welcome dialog
sleep 3
echo "open ${1:-Glacial_P}" > "$G/bl2vr_cmd.txt"
sleep 45
echo "in game"
