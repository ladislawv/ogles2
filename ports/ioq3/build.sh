#!/bin/bash
# Builds ioquake3 for AmigaOS 3 (68k) on gl4es and ogles2.library: the main
# executable and the three game modules, in $B/ioq3. Run in WSL after
# `make` and `make gl4es`:
#   bash ports/ioq3/build.sh
# The game's source is not in this repository: it is copied from $IOQ3_SRC
# (Cowcat's AmigaOS 3 port with the PiStorm changes, which has the Amiga
# platform code and the build lists used here; ports/ioq3/sources.sh clones
# it at a pinned commit unless IOQ3_SRC names a tree) into $B/ioq3/src, amiga_glimp.c is replaced
# (ports/ioq3/amiga_glimp.c) and the copy gets ports/ioq3/patches/*.patch;
# <mgl/gl.h> and <mgl/mglmacros.h> resolve to ports/ioq3/include, which
# brings gl4es's GL. ports/ioq3/README.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ln -sfn "$(cd "$HERE/../.." && pwd)" "$HOME/.ogles2-repo" # a path without the space of "- CPP"
REPO=$HOME/.ogles2-repo
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
. "$HERE/sources.sh" # IOQ3_SRC
GL4ES_SRC=${GL4ES_SRC:-$HOME/gl4es}
GCC=$TC/bin/m68k-amigaos-gcc
AS=$TC/bin/m68k-amigaos-as
LIBM020=$TC/m68k-amigaos/clib2/lib/libm020
ROOT=$B/ioq3/src
OUT=$B/ioq3/obj
CGOUT=$OUT/cgame
QAOUT=$OUT/qagame
UIOUT=$OUT/q3_ui
BUILDOUT=$B/ioq3
[ -f "$B/gl4es/libgl4es.a" ] || { echo "run make gl4es first (B=$B)"; exit 1; }

mkdir -p "$ROOT"
# The game's sources (code/ and ui/: not the other port's objects and binaries).
rsync -a --delete "$IOQ3_SRC/code/" "$ROOT/code/"
rsync -a --delete "$IOQ3_SRC/ui/" "$ROOT/ui/" # menudef.h (code/ui/ui_shared.h)
cp "$HERE/amiga_glimp.c" "$ROOT/code/amiga/amiga_glimp.c"
# Fixes to the copy (the source tree is left as it is). 0001: a multitexture
# stage drew with the colour array set last (often a dynamic light's).
for p in "$HERE"/patches/*.patch; do
    (cd "$ROOT" && git apply "$p") || { echo "ioq3: $p does not apply"; exit 1; }
done
rm -rf "$OUT"
mkdir -p "$OUT" "$CGOUT" "$QAOUT" "$UIOUT"

# The MiniGL library port's flags, without USE_MINIGL_LIBRARY, and with the
# library's -fbbb set: bebbo's default optimiser pass e drops the fintrz of
# float-to-integer conversions (CONTRIBUTING.md, Traps), which made
# renderergl2 pack colours of 1.0 as 0.
CFLAGS="-std=c99 -O2 -finline-functions -mcpu=68020 -m68881 -mcrt=clib2 -fbbb=abcfhilmnprsz0 \
-fno-strict-aliasing -fno-builtin-cos -fno-builtin-sin \
-DAMIGAOS -DNDEBUG -DBOTLIB -DUSE_LOCAL_HEADERS -DDLL -DNO_VM_COMPILED -DUSE_GL4ES"
# ports/ioq3/include first: <mgl/gl.h> is gl4es's GL there.
GLINC="-I$REPO/ports/ioq3/include -I$GL4ES_SRC/include -I$REPO/ports/gl4es/include -I$REPO/include -I$ROOT/code/amiga"

cc() { # source object [extra flags...]
    local src=$1 obj=$2
    shift 2
    $GCC $CFLAGS "$@" -c "$src" -o "$obj" || { echo "FAILED: $src"; exit 1; }
}

cd "$ROOT/code/zlib"
for f in adler32 crc32 inffast inflate inftrees zutil; do cc $f.c "$OUT/$f.o"; done
cd "$ROOT/code/jpeg-8c"
for f in jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff \
         jcinit jcmainct jcmarker jcmaster jcomapi jcparam jcprepct jcsample \
         jctrans jdapimin jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor \
         jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct \
         jdsample jdtrans jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst \
         jidctint jmemmgr jmemnobs jquant1 jquant2 jutils; do cc $f.c "$OUT/$f.o"; done
cd "$ROOT/code/qcommon"
for f in cm_load cm_patch cm_polylib cm_test cm_trace cmd common cvar \
         files huffman md4 msg net_chan net_ip q_math \
         q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 \
         q_shared7 q_shared8 md5 puff unzip ioapi vm vm_interpreted; do cc $f.c "$OUT/$f.o"; done
cd "$ROOT/code/server"
for f in sv_bot sv_ccmds sv_client sv_game sv_init sv_main sv_net_chan \
         sv_snapshot sv_world; do cc $f.c "$OUT/$f.o"; done
cd "$ROOT/code/botlib"
for f in be_aas_bspq3 be_aas_cluster be_aas_debug be_aas_entity be_aas_file \
         be_aas_main be_aas_move be_aas_optimize be_aas_reach be_aas_route \
         be_aas_routealt be_aas_sample be_ai_chat be_ai_gen \
         be_ai_goal be_ai_move be_ai_weap be_ai_weight be_ea be_interface \
         l_crc l_libvar l_log l_memory l_precomp l_script l_struct; do cc $f.c "$OUT/$f.o"; done
# The other port's pin: be_ai_char.c needs -O0 on this toolchain (bots).
$GCC $(echo "$CFLAGS" | sed 's/-O2 -finline-functions/-O0/') -c be_ai_char.c -o "$OUT/be_ai_char.o"
cd "$ROOT/code/renderer"
for f in tr_animation tr_backend tr_bsp tr_cmds tr_curve tr_flares tr_font \
         tr_image tr_init tr_light tr_main tr_marks tr_mesh tr_model \
         tr_noise tr_scene tr_shade tr_shade_calc tr_shader tr_shadows \
         tr_sky tr_surface tr_world tr_image_bmp tr_image_tga tr_image_jpg \
         tr_image_png tr_image_pcx; do cc $f.c "$OUT/$f.o" $GLINC; done
cd "$ROOT/code/client"
for f in cl_cgame cl_cin cl_console cl_input cl_keys cl_main cl_net_chan \
         cl_parse cl_scrn cl_ui cl_avi snd_adpcm snd_dma snd_mem snd_mix \
         snd_wavelet snd_main snd_codec snd_codec_wav; do cc $f.c "$OUT/$f.o"; done
cd "$ROOT/code/amiga"
for f in amiga_main amiga_input amiga_mhandler amiga_snd amiga_snd_ahi amiga_glimp amiga_shared mglv3d_log_noop; do
    cc $f.c "$OUT/$f.o" $GLINC
done
$AS -m68020 -m68881 "$ROOT/code/amiga/snd_int68k_gas.s" -o "$OUT/snd_int68k.o"
cc "$ROOT/code/amiga/dll/dll.c" "$OUT/dll.o" $GLINC

# The game modules: no GL.
cd "$ROOT/code/q3_ui"
for f in ui_addbots ui_atoms ui_cdkey ui_cinematics ui_confirm ui_connect \
         ui_controls2 ui_credits ui_demo2 ui_display ui_gameinfo ui_ingame \
         ui_loadconfig ui_main ui_menu ui_mfield ui_mods ui_network \
         ui_options ui_playermodel ui_players ui_playersettings \
         ui_preferences ui_qmenu ui_removebots ui_saveconfig ui_serverinfo \
         ui_servers2 ui_setup ui_sound ui_sparena ui_specifyserver \
         ui_specifyleague ui_splevel ui_sppostgame ui_spskill \
         ui_startserver ui_team ui_teamorders ui_video ui_login \
         ui_rankings ui_spreset ui_rankstatus ui_signup; do cc $f.c "$UIOUT/$f.o"; done
cc "$ROOT/code/ui/ui_syscalls.c" "$UIOUT/ui_syscalls.o"
cc "$ROOT/code/game/bg_misc.c" "$UIOUT/bg_misc.o"
cc "$ROOT/code/amiga/amiga_ui.c" "$UIOUT/amiga_ui.o"
cd "$ROOT/code/cgame"
for f in cg_main cg_consolecmds cg_draw cg_drawtools cg_effects cg_ents \
         cg_event cg_info cg_localents cg_marks cg_players cg_playerstate \
         cg_predict cg_scoreboard cg_servercmds cg_snapshot cg_syscalls \
         cg_view cg_weapons cg_particles; do cc $f.c "$CGOUT/$f.o"; done
for f in bg_lib bg_misc bg_pmove bg_slidemove; do cc "$ROOT/code/game/$f.c" "$CGOUT/$f.o"; done
cc "$ROOT/code/ui/ui_shared.c" "$CGOUT/ui_shared.o"
cc "$ROOT/code/amiga/amiga_cgame.c" "$CGOUT/amiga_cgame.o"
cc "$ROOT/code/amiga/amiga_game.c" "$QAOUT/amiga_game.o"
cd "$ROOT/code/game"
for f in g_main ai_chat ai_cmd ai_dmnet ai_dmq3 ai_main ai_team ai_vcmd \
         bg_misc bg_pmove bg_slidemove g_active g_arenas g_bot g_client \
         g_cmds g_combat g_items g_mem g_misc g_missile g_mover g_session \
         g_spawn g_svcmds g_syscalls g_target g_team g_trigger g_utils \
         g_weapon; do cc $f.c "$QAOUT/$f.o"; done
for d in "$UIOUT" "$CGOUT" "$QAOUT"; do
    for f in q_math q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 q_shared7 q_shared8; do
        cc "$ROOT/code/qcommon/$f.c" "$d/$f.o"
    done
    for f in dll dllimport dllstartup; do cc "$ROOT/code/amiga/dll/$f.c" "$d/$f.o"; done
done

objs() { local dir=$1; shift; for f in "$@"; do echo -n " $dir/$f.o"; done; }
MAIN=$(objs "$OUT" be_aas_bspq3 be_aas_cluster be_aas_debug be_aas_entity be_aas_file be_aas_main \
  be_aas_move be_aas_optimize be_aas_reach be_aas_route be_aas_routealt be_aas_sample \
  be_ai_char be_ai_chat be_ai_gen be_ai_goal be_ai_move be_ai_weap be_ai_weight be_ea \
  be_interface l_crc l_libvar l_log l_memory l_precomp l_script l_struct \
  cl_avi cl_cgame cl_cin cl_console cl_input cl_keys cl_main cl_net_chan cl_parse \
  cl_scrn cl_ui snd_adpcm snd_codec snd_codec_wav snd_dma snd_main snd_mem snd_mix snd_wavelet \
  jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit jcmainct \
  jcmarker jcmaster jcomapi jcparam jcprepct jcsample jctrans jdapimin jdapistd jdarith \
  jdatadst jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster \
  jdmerge jdpostct jdsample jdtrans jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst \
  jidctint jmemmgr jmemnobs jquant1 jquant2 jutils \
  adler32 crc32 huffman inffast inflate inftrees ioapi puff unzip zutil \
  cm_load cm_patch cm_polylib cm_test cm_trace cmd common cvar files md4 md5 msg \
  net_chan net_ip vm vm_interpreted \
  tr_animation tr_backend tr_bsp tr_cmds tr_curve tr_flares tr_font tr_image \
  tr_image_bmp tr_image_jpg tr_image_pcx tr_image_png tr_image_tga tr_init tr_light tr_main \
  tr_marks tr_mesh tr_model tr_noise tr_scene tr_shade tr_shade_calc tr_shader tr_shadows \
  tr_sky tr_surface tr_world \
  sv_bot sv_ccmds sv_client sv_game sv_init sv_main sv_net_chan sv_snapshot sv_world \
  amiga_glimp amiga_input amiga_main amiga_mhandler amiga_shared amiga_snd amiga_snd_ahi snd_int68k dll \
  q_math q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 q_shared7 q_shared8 mglv3d_log_noop)
# gl4es before libogles2.a (ports/gl4es/README.md).
$GCC -mcpu=68020 -m68881 -mcrt=clib2 -o "$BUILDOUT/quake3-gl4es" $MAIN \
    -L"$B/gl4es" -L"$B" -lgl4es -logles2 -L"$LIBM020" -lm881 -lm -lc -lamiga 2>&1 | grep -v lang_reloc || true
[ -f "$BUILDOUT/quake3-gl4es" ] || { echo "link failed"; exit 1; }

CG=$(objs "$CGOUT" cg_main cg_consolecmds cg_draw cg_drawtools cg_effects cg_ents cg_event cg_info \
  cg_localents cg_marks cg_players cg_playerstate cg_predict cg_scoreboard cg_servercmds \
  cg_snapshot cg_syscalls cg_view cg_weapons cg_particles bg_lib bg_misc bg_pmove bg_slidemove \
  q_math q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 q_shared7 q_shared8 \
  ui_shared amiga_cgame dll dllimport dllstartup)
QA=$(objs "$QAOUT" amiga_game g_main ai_chat ai_cmd ai_dmnet ai_dmq3 ai_main ai_team ai_vcmd \
  bg_misc bg_pmove bg_slidemove g_active g_arenas g_bot g_client g_cmds g_combat g_items g_mem \
  g_misc g_missile g_mover g_session g_spawn g_svcmds g_syscalls g_target g_team g_trigger \
  g_utils g_weapon q_math q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 q_shared7 \
  q_shared8 dll dllimport dllstartup)
UI=$(objs "$UIOUT" ui_addbots ui_atoms ui_cdkey ui_cinematics ui_confirm ui_connect ui_controls2 \
  ui_credits ui_demo2 ui_display ui_gameinfo ui_ingame ui_loadconfig ui_main ui_menu ui_mfield \
  ui_mods ui_network ui_options ui_playermodel ui_players ui_playersettings ui_preferences \
  ui_qmenu ui_removebots ui_saveconfig ui_serverinfo ui_servers2 ui_setup ui_sound ui_sparena \
  ui_specifyserver ui_specifyleague ui_splevel ui_sppostgame ui_spskill ui_startserver ui_team \
  ui_teamorders ui_video ui_login ui_rankings ui_spreset ui_rankstatus ui_signup ui_syscalls \
  q_math q_shared1 q_shared2 q_shared3 q_shared4 q_shared5 q_shared6 q_shared7 q_shared8 \
  bg_misc amiga_ui dll dllimport dllstartup)
for m in "cgame68k.dll:$CG" "qagame68k.dll:$QA" "ui68k.dll:$UI"; do
    $GCC -mcpu=68020 -m68881 -mcrt=clib2 -o "$BUILDOUT/${m%%:*}" ${m#*:} -L"$LIBM020" -lm881 -lm -lamiga \
        2>&1 | grep -v lang_reloc || true
done
ls -l "$BUILDOUT"/quake3-gl4es "$BUILDOUT"/*.dll
