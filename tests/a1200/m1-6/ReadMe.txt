ogles2 M1 run 6
===============

What this is
------------
The first triangles. The GPU draws smooth-shaded triangles with the
library's built-in program and the test reads back what is shown:
colours and orientation, a constant colour, the transformation matrix,
culling, the depth test, viewport, scissor, blending, colour mask, strips
and fans, lines and points, then 100 frames of 200 triangles timed. First
on a hidden bitmap, then in a window, then on the whole screen.

Each picture stays one second. Normally it runs remotely (amictl); it works
by hand too. Please do not run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-6
       Execute Run-M1
2. Four steps, about a minute. Read the Shell before each window opens.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-6
       Execute Run-M1 FROM=3

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_draw          draws triangles, then checks the pixels
bin/m1_clear         the step 1 test, as a regression check
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
