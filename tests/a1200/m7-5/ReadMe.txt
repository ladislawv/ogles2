ogles2 M7 run 5
===============

What this is
------------
A test of drawing into half-float pictures (EXT_color_buffer_half_float),
which OpenLara's water needs: values above 1 and below 0 must be kept.
Several render targets are tested again, and the earlier drawing, texture
and framebuffer tests run first, as before.

Everything draws on hidden bitmaps: there is nothing to watch. The whole
run takes less than a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m7-5
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line, so its last lines show how far it got. To go on after the step
that hung:   Execute Run-M1 FROM=<the next step>

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m7_half          the half-float render targets test
bin/m7_mrt           the render targets test
bin/m1_draw, m2_texture, m5_fbo   the earlier tests
compiler/v3dglslc    the shader compiler (m7_half's and m7_mrt's programs
                     are compiled on the Amiga, into cache/)
bin/*.v3dp, *.v3dw   the other tests' compiled shaders
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
