#!/bin/bash
# Fills the ogles2-m3-1 package directory ($1): the on-target compiler
# (plan 8.4, M3). The package has the compiler (libs/v3dglslc) and no .v3dp
# files at all: every program is compiled on the Amiga, into a cache
# directory of the package (OGLES2Cache:) that the results keep, so the PC
# can compare the binaries with the offline compiler's. Called by
# ../build_package.sh. Run-M1 is written here, one step per program.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BS=${BS:-$HOME/ogles2-build/m68k-sync}
B=${B:-$HOME/ogles2-build/m68k}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null # the test programs' sources and offline binaries
B="$B" bash "$REPO/tests/corpus/build_corpus.sh" > /dev/null 2>&1 || { echo "build_corpus.sh failed"; exit 1; }
C=$B/corpus
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/data" "$PKG/shaders"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$BS/cmd/OGLES2Log" "$BS/tests/m2_link" "$BS/tests/m2_elements" "$BS/tests/m2_texture" \
   "$BS/tests/m1_draw" "$PKG/bin/"
cp "$B"/v3dp/src/*.vert "$B"/v3dp/src/*.frag "$PKG/shaders/"
cp "$C"/data/*.tga "$PKG/data/"
while read -r name shot; do
    [ -n "$name" ] && cp "$C/amiga/$name" "$PKG/bin/"
done < "$C/steps.txt"

look() {
    case $1 in
    es2gears) echo "three gears, red, green and blue, turning" ;;
    Hello_Triangle) echo "a red triangle on black" ;;
    Simple_VertexShader) echo "a red cube turning" ;;
    Simple_Texture2D) echo "a square in four colours: red, green, blue, yellow" ;;
    MipMap2D) echo "two red and blue chequered floors running into the distance" ;;
    TextureWrap) echo "three red and blue squares: chequers, a cross, mirrored chequers" ;;
    Simple_TextureCubemap) echo "a ball with six coloured sides" ;;
    MultiTexture) echo "a stone wall with a light on it" ;;
    Stencil_Test) echo "four squares: red, green, blue, yellow" ;;
    Multisample) echo "the whole window yellow" ;;
    ParticleSystem) echo "puffs of coloured particles, a new one every second" ;;
    *) echo "see ReadMe.txt" ;;
    esac
}

n=$(( $(grep -c . "$C/steps.txt") + 4 ))
{
cat <<'EOF'
; Run-M1 - ogles2 M3 run 1: shaders compiled on the Amiga. See ReadMe.txt.
;   CD to this directory, then:   Execute Run-M1
;   After a reboot in the middle:  Execute Run-M1 FROM=<step>
; Nothing is installed: libs/ is added to LIBS: for the run and removed at the
; end; OGLES2Cache: points at cache/ for the run. Output goes to results/.
.key FROM/K
.def FROM 1
.bra {
.ket }

FailAt 21
If NOT EXISTS libs/ogles2.library
  Echo "Please CD into the ogles2-m3-1 directory first,"
  Echo "then run:  Execute Run-M1"
  Quit 20
EndIf

Assign M3R1: ""
If NOT EXISTS M3R1:results
  MakeDir M3R1:results
EndIf
If NOT EXISTS M3R1:cache
  MakeDir M3R1:cache
EndIf
; A run from the start compiles everything again: empty the cache.
If NOT {FROM} GT 1 VAL
  Delete M3R1:cache/#? QUIET >NIL:
EndIf
Echo "ogles2 M3 run 1, starting at step {FROM}" >>M3R1:results/00-progress.txt
Date >>M3R1:results/00-progress.txt
Assign LIBS: M3R1:libs ADD
Assign OGLES2Cache: M3R1:cache
; Expunge libraries nobody has open: an ogles2.library left in memory by an
; earlier run would otherwise be used instead of this package's.
Avail FLUSH >NIL:
SetEnv OGLES2/LogFile M3R1:results/ogles2.log
SetEnv CORPUS_FRAMES 60

If NOT {FROM} GT 1 VAL
  Echo "Step 1: linking and uniforms, every program compiled here; the first"
  Echo "        start of the compiler takes a few seconds (nothing to see)"
  M3R1:bin/m2_link >M3R1:results/01-link.txt
  Echo "step 1 done" >>M3R1:results/00-progress.txt
EndIf

If NOT {FROM} GT 2 VAL
  Echo "Step 2: glDrawElements and an indexed uniform array (nothing to see)"
  M3R1:bin/m2_elements >M3R1:results/02-elements.txt
  Echo "step 2 done" >>M3R1:results/00-progress.txt
EndIf

If NOT {FROM} GT 3 VAL
  Echo "Step 3: textures on a hidden bitmap (nothing to see)"
  M3R1:bin/m2_texture BITMAP QUICK >M3R1:results/03-texture.txt
  Echo "step 3 done" >>M3R1:results/00-progress.txt
EndIf

If NOT {FROM} GT 4 VAL
  Echo "Step 4: the triangle test on a hidden bitmap (nothing to see)"
  M3R1:bin/m1_draw BITMAP QUICK >M3R1:results/04-draw.txt
  Echo "step 4 done" >>M3R1:results/00-progress.txt
EndIf

; The samples open their images from the current directory.
CD M3R1:data

EOF
i=4
while read -r name shot; do
    [ -n "$name" ] || continue
    i=$((i + 1))
    cat <<EOF
If NOT {FROM} GT $i VAL
  Echo "Step $i of $n: $name - $(look "$name")"
  SetEnv CORPUS_SHOT $shot
  SetEnv CORPUS_OUT M3R1:results/$name.ppm
  M3R1:bin/$name >M3R1:results/$(printf %02d $i)-$name.txt
  Echo "step $i done" >>M3R1:results/00-progress.txt
EndIf

EOF
done < "$C/steps.txt"
cat <<'EOF'
CD M3R1:
UnSetEnv CORPUS_FRAMES
UnSetEnv CORPUS_SHOT
UnSetEnv CORPUS_OUT
UnSetEnv OGLES2/LogFile
; What the compiler wrote, for the comparison with the offline compiler.
Copy M3R1:cache M3R1:results/cache ALL QUIET
List M3R1:cache >M3R1:results/cache-list.txt
; Expunging the library stops the compiler: it must be gone afterwards.
Assign LIBS: M3R1:libs REMOVE
Avail FLUSH >NIL:
Wait 2
Status >M3R1:results/status-after.txt
Assign OGLES2Cache:
If NOT EXISTS M3R1:results/notes.txt
  Copy M3R1:notes-template.txt M3R1:results/notes.txt
EndIf
List M3R1:results >M3R1:results/99-files.txt
Echo "finished" >>M3R1:results/00-progress.txt
Date >>M3R1:results/00-progress.txt
Assign M3R1:
Echo ""
Echo "Done. Results are in results/."
EOF
} > "$PKG/Run-M1"
