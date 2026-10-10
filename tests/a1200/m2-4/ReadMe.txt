ogles2 M2 run 4
===============

What this is
------------
Buffer objects: vertex data and index lists that a program hands to the
library once and draws from many times, as nearly every OpenGL ES 2.0
program does. Then the earlier tests again as regression checks. All on
hidden bitmaps: nothing to watch, about a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m2-4
       Execute Run-M1
2. Five steps, about a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m2-4
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m2_link          linking and uniforms
bin/m2_texture       textures
bin/m2_buffers       buffer objects
bin/m2_elements      glDrawElements, indexed uniform arrays
bin/m1_draw          the triangle test
bin/*.v3dp           the test programs' binaries
shaders/             the test programs' GLSL
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
