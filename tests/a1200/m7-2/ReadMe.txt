ogles2 M7 run 2
===============

What this is
------------
A test of S3TC compressed textures (DXT1, DXT3, DXT5), which ogles2 now
keeps compressed: the GPU decodes them as it draws. The test draws known
textures and compares every texel with what the S3TC specification says it
should be; then the earlier texture test runs as before.

Everything draws on hidden bitmaps: there is nothing to watch. The whole
run takes less than a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m7-2
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
bin/m7_s3tc          the S3TC test
bin/m2_texture       the earlier texture test
bin/*.v3dp, *.v3dw   the tests' compiled shaders
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
