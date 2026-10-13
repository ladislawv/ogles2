ogles2 M6 run 6
===============

What this is
------------
The OpenGL ES extensions ogles2 offers (the list AmigaOS 4's ogles2 has,
and a few more): texture filtering and level-of-detail controls, reading
pixels as BGRA, shader derivatives, wireframe drawing, program binaries,
float and depth textures, rectangle textures. Then the earlier tests again
as regression checks. All on hidden bitmaps: nothing to watch, about half
a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-6
       Execute Run-M1
2. Five steps, about a minute (the last one starts the shader compiler).
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m6-6
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m6_ext           the extensions
bin/m2_texture       textures
bin/m1_draw          the triangle test
bin/m5_fbo           framebuffer objects
bin/m4_depth         depth and stencil across a mid-frame flush
bin/m2_link          linking and uniforms
bin/m2_elements      glDrawElements, indexed uniform arrays
bin/m2_buffers       buffer objects
bin/*.v3dp           the test programs' binaries
bin/*.v3dw           their variants for float and depth textures
bin2/m6_ext          the extension test without binaries (step 5)
compiler/v3dglslc    the shader compiler (in LIBS: for step 5 only)
shaders/             the test programs' GLSL
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
