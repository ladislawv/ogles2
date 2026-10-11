ogles2 M6 run 2
===============

What this is
------------
Quake III Arena (ioquake3, the MiniGL port's source) built on gl4es and
ogles2.library instead of MiniGL. It plays the game's demo "four" as a
timedemo (as fast as it can) at 640x480 on a screen of its own, twice:
the first time gl4es's shaders are compiled (the screen may stay black for
a minute), the second time they come from the cache. The game prints the
frames per second at the end.

The game data stays where it is installed (Stuff:Quake3-68k-beta10/
Quake3-68k-beta10); nothing there is changed: the game's config and logs
go to home/ in this package. Sound is off.

Please do not run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-2
       Execute Run-M1
   With the game installed elsewhere:
       Execute Run-M1 Q3DATA=<the directory that holds baseq3>
2. One run of the demo, a few minutes: it stops after about 230 of its
   1260 frames. The whole demo:  Execute Run-M1 FRAMES=0
3. Send me the whole results directory.

To play the game instead (not timed; frame rate shown, no sound):
       Execute Play-Quake
   Quit from the game's menu (Escape, Exit).

If the machine hangs or crashes
-------------------------------
Reboot and send me the results directory as it is.

Contents
--------
Run-M1                    the test script
Play-Quake                the game to play
libs/ogles2.library       the library (logs every line at once)
libs/v3dglslc             the shader compiler (started by the library)
bin/quake3-gl4es          the game on gl4es
bin/*.dll, home/baseq3/   the game's modules, built from the same source
notes-template.txt        copied to results/notes.txt at the end
