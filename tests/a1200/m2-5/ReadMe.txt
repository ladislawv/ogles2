ogles2 M2 run 5
===============

What this is
------------
Real OpenGL ES 2.0 programs, compiled unchanged for the Amiga: es2gears
(the gears demo from Mesa) and ten samples from the book "OpenGL ES 2.0
Programming Guide". Each one opens its own window on the Workbench screen
for two or three seconds, then closes it. Before each one the Shell says
what it should show. One picture of each is saved and compared with the
same program drawn on a PC.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m2-5
       Execute Run-M1
2. Eleven steps, about a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m2-5
       Execute Run-M1 FROM=4

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/                 the eleven programs and their shader binaries (.v3dp)
data/                the images two of the samples load
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
