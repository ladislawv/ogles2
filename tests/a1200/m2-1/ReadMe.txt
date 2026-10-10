ogles2 M2 run 1
===============

What this is
------------
The first run with real compiled GLSL programs. Until now every program
drew with one built-in shader set from PiStorm3D. Now the library looks
up each program's binary by a hash of its sources (files <hash>.v3dp,
made on the PC by the offline compiler), loads it, and draws with it.

Three steps: linking programs and setting their uniforms (nothing on the
screen), the triangle test on the three surfaces with a compiled
program, and the 10,000-frame soak with it.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m2-1
       Execute Run-M1
2. Three steps, under a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m2-1
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m2_link          linking and uniforms
bin/m1_draw          the triangle test
bin/m1_soak          10,000 frames
bin/*.v3dp           the test programs' binaries
shaders/             the test programs' GLSL
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
