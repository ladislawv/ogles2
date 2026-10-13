ogles2 M6 run 8
===============

What this is
------------
A test of the GPU's texture formatting unit (TFU), which now makes a
texture's smaller copies (mipmaps) instead of the 68k. It runs the same
test twice: first with the old way (the 68k), then with the TFU, and
compares every copy with what it should be. Then the earlier texture and
framebuffer tests run again.

Everything draws on hidden bitmaps: there is nothing to watch. The whole
run takes less than a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-8
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
bin/m6_tfu           the mipmap test
bin/m2_texture       the texture test
bin/m5_fbo           the framebuffer test
bin/*.v3dp, *.v3dw   the tests' compiled shaders
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
