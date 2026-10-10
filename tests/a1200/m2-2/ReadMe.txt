ogles2 M2 run 2
===============

What this is
------------
Textures. Compiled GLSL programs now sample textures: the test draws
textured rectangles and reads back what is shown (orientation, every
upload format, filtering, wrap modes, mipmaps, two textures at once, a
texture changed in the middle of a frame), first on a hidden bitmap, then
in a window you can watch. Before that the program-linking test, after it
the triangle test as a regression check.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m2-2
       Execute Run-M1
2. Three steps, about a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m2-2
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m2_link          linking and uniforms
bin/m2_texture       textures
bin/m1_draw          the triangle test
bin/*.v3dp           the test programs' binaries
shaders/             the test programs' GLSL
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
