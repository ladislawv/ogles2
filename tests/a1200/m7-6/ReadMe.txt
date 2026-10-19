ogles2 M7 run 6
===============

What this is
------------
A test of copying the picture into a texture (glCopyTexImage2D and
glCopyTexSubImage2D), which OpenLara does every frame for its water. The
copies are checked first, from a hidden picture and from a small window;
then a 640x480 window measures how much a whole-screen copy costs a frame.

The small window shows four coloured quarters for about a second; the big
one moving rectangles with a strip at the bottom for a few seconds. The
whole run takes less than a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m7-6
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is. To go on after the step
that hung:   Execute Run-M1 FROM=<the next step>

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library
bin/m7_copy          the copy test
bin/*.v3dp, *.v3dw   the test's compiled shaders
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
