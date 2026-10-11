ogles2 M6 run 4: SDL3 on ogles2.library
=======================================

What this tests
---------------
SDL3 for AmigaOS 3 (Steffen Haeuser's SDL3_68k, from Aminet's SDL_68k) with
a new OpenGL ES 2.0 backend on ogles2.library (ports/sdl3): a program that
asks SDL for an OpenGL ES 2 context gets one from ogles2, and SDL's GLES2
renderer (its hardware 2D drawing) runs on it. Desktop OpenGL in SDL still
goes to minigl.library, as before.

The programs are SDL3's own tests: testgles2 (a GLES2 program), testsprite,
testrendercopyex and testdraw (SDL's 2D renderer). Each runs 6 seconds and
quits by itself.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-4
       Execute Run-M1
2. Six steps, about a minute. The Shell says what you should see at each.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and continue with the next step, for example:
       CD <...>/ogles2-m6-4
       Execute Run-M1 FROM=3

Contents
--------
Run-M1                 the test script
libs/ogles2.library    the library (logs every line at once)
libs/v3dglslc          the shader compiler (started by the library)
bin/                   the SDL3 test programs, icon.png, sample.png
