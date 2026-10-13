ogles2 M6 run 7
===============

What this is
------------
glmark2, the usual OpenGL ES 2.0 benchmark, compiled for the Amiga
(68020 + 68881) and drawn by ogles2.library. It runs glmark2's standard
list of 33 scenes in an 800x600 window on the Workbench screen and prints
a score for each, and a total at the end.

By default each scene runs for 2 seconds (about 2 minutes in all); with
DURATION=10 it runs as glmark2 normally does. The first time a scene
starts, its shaders are compiled on the Amiga, which shows as a pause;
they are kept in cache/ and later runs start at once.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program (Quake, MiniGL) at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-7
       Execute Run-M1
   The real benchmark:        Execute Run-M1 DURATION=10
   Another window size:       Execute Run-M1 SIZE=640x480
   Esc in glmark2's window, or Ctrl-C, ends it early.
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line, so its last lines show how far it got.

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
libs/v3dglslc        the shader compiler
bin/glmark2-es2      glmark2 2023.01 (ports/glmark2)
bin/data/            glmark2's models, shaders and textures
bin/OGLES2Log        writes the library's log file on request
cache/               the compiled shaders, filled as the scenes run
notes-template.txt   copied to results/notes.txt at the end
