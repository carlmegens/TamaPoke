#!/bin/bash
# Rebuilds the kids' manual (manual/TamaPoke-manual-EN.pdf and -manuel-FR.pdf)
# from the REAL firmware: cap.cpp compiles TamaPoke.ino against the emulator's
# stubs, sets each screen up the way a player reaches it, and dumps the panel.
# So a UI change is one re-run away from an up-to-date manual.
#
#   bash tools/manual/build.sh
#
# Needs g++, Python with Pillow + reportlab (pip install pillow reportlab), and
# Comic Sans. The sprites come from web/sprites-kanto.pak, so no SD card or
# sprite pipeline is needed. The text lives in texts.py, both languages.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EMU="$ROOT/tools/emu"
WORK="$HERE/work"
mkdir -p "$WORK/shots" "$ROOT/manual"

# the sketch as C++, with the prototypes arduino-cli would have generated
(cd "$EMU" && python genproto.py "$ROOT/TamaPoke.ino" >/dev/null)   # genproto writes proto.h into the cwd
{ echo '#include "proto.h"'; cat "$ROOT/TamaPoke.ino"; } > "$EMU/sketch.cpp"

g++ -std=c++17 -O1 -w -I"$EMU" -I"$ROOT" -DSPRITE_DIR='"x"' -o "$WORK/cap" \
  "$HERE/cap.cpp" "$ROOT"/{gbsynth,pet,i18n,party,battle,link,save,dexbox}.cpp \
  "$EMU"/{sketch,host_impl,font,clock}.cpp

[ -d "$WORK/sd/mons" ] || python "$HERE/unpack_pak.py" "$ROOT/web/sprites-kanto.pak" "$WORK/sd"

SCENES="region starter egg main feed sleep menu card1 card2 train player stickers party
        gympick gyms fight fightmoves win dexpick dex dexdetail quiz quizright quizdone
        clock date options birthday explmap explscene explocean explvolcano wild caught keep dexbox sheetxp"
cd "$WORK"
for s in $SCENES; do
  for l in 1 2 3; do                     # LANG_EN, LANG_FR, LANG_DE
    rm -rf tamapoke* ./*.nvs             # every capture starts from a fresh save
    ./cap "$s" "$l" "shots/${s}_$l.ppm" "$WORK/sd/mons" >/dev/null
  done
done
python - <<'EOF'
import glob, os
from PIL import Image
for p in glob.glob('shots/*.ppm'):
    Image.open(p).save(p[:-4] + '.png'); os.remove(p)
EOF

python "$HERE/build.py" "$WORK" "$ROOT/manual"
