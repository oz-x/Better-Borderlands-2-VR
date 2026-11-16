#!/bin/bash
# Asks the mod for the next eye images and stitches them into build/pair.png.
G="W:/SteamLibrary/steamapps/common/Borderlands 2/Binaries/Win32"
R="$(cd "$(dirname "$0")/.." && pwd)"
rm -f "$G/bl2vr_left.bmp" "$G/bl2vr_right.bmp"
touch "$G/bl2vr_dump.txt"
for i in $(seq 1 20); do sleep 0.5; [ -f "$G/bl2vr_right.bmp" ] && break; done
sleep 0.5
python - "$G" "$R/build/pair.png" "${1:-1400}" <<'PY'
import sys
from PIL import Image
g, out, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
ims = [Image.open(f"{g}/bl2vr_{n}.bmp").convert("RGB") for n in ("left", "right")]
w, h = ims[0].size
o = Image.new("RGB", (w * 2 + 10, h)); o.paste(ims[0], (0, 0)); o.paste(ims[1], (w + 10, 0))
o.thumbnail((size, size)); o.save(out); print("pair", w, h)
PY
