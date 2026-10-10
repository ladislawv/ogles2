#!/bin/bash
# Fills the ogles2-m2-5 package directory ($1): the corpus (es2gears and the
# OpenGL ES 2.0 Programming Guide samples, tests/corpus). Called by
# ../build_package.sh. The library is the sync-logging build; the programs
# and their .v3dp binaries go to bin/ (PROGDIR:), the samples' TGA images
# to data/ (the samples open them from the current directory). Run-M1 is
# written here, one step per program, from the corpus's steps.txt.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BS=${BS:-$HOME/ogles2-build/m68k-sync}
B=${B:-$HOME/ogles2-build/m68k}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/tests/corpus/build_corpus.sh" > /dev/null 2>&1 || { echo "build_corpus.sh failed"; exit 1; }
C=$B/corpus
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/data"
cp "$BS/ogles2.library" "$PKG/libs/"
cp "$BS/cmd/OGLES2Log" "$PKG/bin/"
cp "$C"/v3dp/*.v3dp "$PKG/bin/"
cp "$C"/data/*.tga "$PKG/data/"

# What each program shows (the PC's reference images, tests/corpus).
look() {
    case $1 in
    es2gears) echo "three gears, red, green and blue, turning" ;;
    Hello_Triangle) echo "a red triangle on black" ;;
    Simple_VertexShader) echo "a red cube turning" ;;
    Simple_Texture2D) echo "a square in four colours: red, green, blue, yellow" ;;
    MipMap2D) echo "two red and blue chequered floors running into the distance" ;;
    TextureWrap) echo "three red and blue squares: chequers, a cross, mirrored chequers" ;;
    Simple_TextureCubemap) echo "a ball with coloured sides (expected black for now: no cube maps yet)" ;;
    MultiTexture) echo "a stone wall with a light on it" ;;
    Stencil_Test) echo "four squares: red, green, blue, yellow" ;;
    Multisample) echo "the whole window yellow" ;;
    ParticleSystem) echo "puffs of coloured particles, a new one every second" ;;
    *) echo "see ReadMe.txt" ;;
    esac
}

n=$(grep -c . "$C/steps.txt")
{
cat <<'EOF'
; Run-M1 - ogles2 M2 run 5: the corpus. See ReadMe.txt.
;   CD to this directory, then:   Execute Run-M1
;   After a reboot in the middle:  Execute Run-M1 FROM=<step>
; Nothing is installed: libs/ is added to LIBS: for the run and removed at the
; end. Output goes to results/.
.key FROM/K
.def FROM 1
.bra {
.ket }

FailAt 21
If NOT EXISTS libs/ogles2.library
  Echo "Please CD into the ogles2-m2-5 directory first,"
  Echo "then run:  Execute Run-M1"
  Quit 20
EndIf

Assign M2R5: ""
If NOT EXISTS M2R5:results
  MakeDir M2R5:results
EndIf
Echo "ogles2 M2 run 5, starting at step {FROM}" >>M2R5:results/00-progress.txt
Date >>M2R5:results/00-progress.txt
Assign LIBS: M2R5:libs ADD
; Expunge libraries nobody has open: an ogles2.library left in memory by an
; earlier run would otherwise be used instead of this package's.
Avail FLUSH >NIL:
SetEnv OGLES2/LogFile M2R5:results/ogles2.log
SetEnv CORPUS_FRAMES 60
; The samples open their images from the current directory.
CD M2R5:data

EOF
i=0
while read -r name shot; do
    [ -n "$name" ] || continue
    i=$((i + 1))
    cat <<EOF
If NOT {FROM} GT $i VAL
  Echo "Step $i of $n: $name - $(look "$name")"
  SetEnv CORPUS_SHOT $shot
  SetEnv CORPUS_OUT M2R5:results/$name.ppm
  M2R5:bin/$name >M2R5:results/$(printf %02d $i)-$name.txt
  Echo "step $i done" >>M2R5:results/00-progress.txt
EndIf

EOF
done < "$C/steps.txt"
cat <<'EOF'
CD M2R5:
UnSetEnv CORPUS_FRAMES
UnSetEnv CORPUS_SHOT
UnSetEnv CORPUS_OUT
UnSetEnv OGLES2/LogFile
Assign LIBS: M2R5:libs REMOVE
If NOT EXISTS M2R5:results/notes.txt
  Copy M2R5:notes-template.txt M2R5:results/notes.txt
EndIf
List M2R5:results >M2R5:results/99-files.txt
Echo "finished" >>M2R5:results/00-progress.txt
Date >>M2R5:results/00-progress.txt
Assign M2R5:
Echo ""
Echo "Done. Results are in results/."
EOF
} > "$PKG/Run-M1"
while read -r name shot; do
    [ -n "$name" ] && cp "$C/amiga/$name" "$PKG/bin/"
done < "$C/steps.txt"
