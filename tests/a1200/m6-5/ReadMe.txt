ogles2 M6 run 5
===============

What this is
------------
Quake III Arena with ioquake3's modern renderer (renderergl2, written for
OpenGL ES 2.0) on ogles2.library directly, without gl4es in between. The
game engine and its modules are the same as in run 2 (Cowcat's Amiga
port); only the renderer differs. It plays the demo "four" as a timedemo,
full screen 640x480, then quits.

The first start compiles about 100 shader programs on the Amiga: the screen
may stay black for a few minutes. They are kept in cache/, so later starts
are quick.

It needs the Quake III data where it is installed on the test machine
(Stuff:Quake3-68k-beta10/Quake3-68k-beta10); nothing is written there.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-5
       Execute Run-M1
   The whole demo:            Execute Run-M1 FRAMES=0
   With sound:                Execute Run-M1 FRAMES=0 SOUND=1
     (a timedemo runs faster than real time: its sound is garbled)
   To play (with sound):      Execute Play-Quake
   With the game elsewhere:   Execute Run-M1 Q3DATA=<the directory with baseq3>
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line, so its last lines show how far it got.

Contents
--------
Run-M1               the test script
Play-Quake           Quake III to play (not timed): Execute Play-Quake
libs/ogles2.library  the library (logs every line at once)
libs/v3dglslc        the shader compiler
bin/quake3-gl2       Quake III with the GLES2 renderer (ports/ioq3-gl2)
bin/*.dll            the game modules (also in home/baseq3)
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
