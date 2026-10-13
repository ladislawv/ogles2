ogles2 M6 run 10
================

What this is
------------
A test of 4x antialiasing (MSAA): the GPU takes four samples per pixel,
so the edges of shapes come out smooth instead of stepped. Programs can
ask for it, and it can be switched on for any program from the Shell:
       SetEnv OGLES2/MSAA 4

The run checks the antialiasing itself, then runs the earlier drawing,
texture and framebuffer tests as before, and once more with antialiasing
switched on for all of them.

Everything draws on hidden bitmaps: there is nothing to watch. The whole
run takes less than a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-10
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
bin/m6_msaa          the antialiasing test
bin/m1_draw, m2_texture, m5_fbo   the earlier tests
bin/*.v3dp, *.v3dw   the tests' compiled shaders
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
