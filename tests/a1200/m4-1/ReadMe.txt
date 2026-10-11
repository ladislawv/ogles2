ogles2 M4 run 1
===============

What this is
------------
A frame with far more drawing than usual: 400,000 small pictures in one
frame, more than the library's lists for one frame hold. Until now such a
frame was thrown away; now the lists continue in more memory. Then the
earlier tests again as regression checks. All on hidden bitmaps: nothing
to watch, one to two minutes.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m4-1
       Execute Run-M1
2. Six steps, one to two minutes.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m4-1
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m2_link          linking and uniforms
bin/m2_texture       textures
bin/m4_stress        the big frame
bin/m4_depth         depth and stencil across a mid-frame flush
bin/m4_setbitmap     aglSetBitmap: one context, several bitmaps
bin/m2_buffers       buffer objects
bin/m2_elements      glDrawElements, indexed uniform arrays
bin/m1_draw          the triangle test
bin/*.v3dp           the test programs' binaries
shaders/             the test programs' GLSL
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
