ogles2 M4 run 2
===============

What this is
------------
A speed comparison: the same three gears (Brian Paul's "gears" demo)
drawn by ogles2.library and by PiStorm3D 29.1's minigl.library, with the
same triangles, the same lighting and the same 640x480 size. Each run
lasts 5 seconds and prints its frames per second.

  Steps 1 and 2: hidden bitmaps, nothing to watch (about 1.5 minutes).
                 Three scene sizes: 800, 8,000 and 32,000 triangles.
  Step 3:        a window on the Workbench, 4 runs of 5 seconds: three
                 gears (red, green, blue) turning, the window title names
                 the library. Both libraries should look the same.

The two libraries take turns: never both at once. The package brings its
own minigl.library 29.1 (used only if LIBS: has none).

Normally it runs remotely (amictl); it works by hand too.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m4-2
       Execute Run-M2
2. Three steps, about 2 minutes.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m4-2
       Execute Run-M2 FROM=2

Contents
--------
Run-M2               the test script
libs/ogles2.library  the library
libs/minigl.library  PiStorm3D 29.1 (Aminet util/libs/PiStorm3D.lha)
bin/gears_gles2      the gears on ogles2 (vertex buffers or client arrays)
bin/gears_mgl        the gears on MiniGL (immediate mode, vertex arrays
                     or display lists)
bin/*.v3dp           the gears' shader binary (and the other tests')
notes-template.txt   copied to results/notes.txt at the end
