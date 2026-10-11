ogles2 M6 run 1
===============

What this is
------------
gl4es on the A1200: a library that turns desktop OpenGL (the kind MiniGL
programs use: glBegin, display lists, fixed-function lighting) into
OpenGL ES 2.0 for ogles2.library. It writes its own shaders while the
program runs, so this package brings ogles2's shader compiler; the first
run compiles them (up to a minute), later runs find them in cache/.

The program draws the same three gears as the speed comparison (run M4-2)
and prints its frames per second.

  Steps 1 and 2: hidden bitmaps, nothing to watch.
  Step 3:        a window on the Workbench, 5 seconds: three gears (red,
                 green, blue) turning, the window titled "gl4es".
  Step 4:        full screen, 5 seconds: the screen switches to the gears
                 and back.

Nothing is installed. Please do not run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-1
       Execute Run-M1
2. Four steps, about two minutes.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m6-1
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
libs/v3dglslc        the shader compiler (started by the library)
bin/gears_gl4es      the gears on gl4es
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
