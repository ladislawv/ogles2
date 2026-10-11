ogles2 M6 run 3: SDL2 on ogles2.library
========================================

What this tests
---------------
SDL2 for AmigaOS 3 (Steffen Haeuser's SDL2_68k, as in Aminet's
dev/lib/SDL_68k) with a new OpenGL ES 2.0 backend on ogles2.library: a
program that asks SDL for an OpenGL ES 2 context gets one from ogles2, and
SDL's GLES2 renderer (its hardware 2D drawing) runs on it. Desktop OpenGL in
SDL still goes to minigl.library, as before.

The programs are SDL's own tests: testgles2 (a GLES2 program), testsprite2,
testrendercopyex and testdraw2 (SDL's 2D renderer). Each runs 6 seconds and
quits by itself. Step 7, rtt_churn, makes testrendercopyex's GL calls itself
and times them one by one.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-3
       Execute Run-M1
2. Seven steps, about a minute and a half. The Shell says what you should see at each.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and continue with the next step, for example:
       CD <...>/ogles2-m6-3
       Execute Run-M1 FROM=3

Contents
--------
Run-M1                 the test script
libs/ogles2.library    the library (logs every line at once)
libs/v3dglslc          the shader compiler (started by the library)
bin/                   the SDL test programs, icon.bmp, sample.bmp
