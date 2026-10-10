ogles2 M1 run 5
===============

What this is
------------
The GPU clears the picture to a colour and shows it, in the four places a
program can draw into: a hidden bitmap, a window ogles2 opens, a window the
program opens, and the whole screen. After every frame the test reads back
what is shown and checks it.

Runs 2 and 3 used, without anyone noticing, the library of run 1 that was
still in memory, with its colour bug. Run 4 (a probe on a hidden bitmap)
loaded the right library and every colour came out exact. This run repeats
run 3 with the right library: everything should pass now. The script
starts with "Avail FLUSH", which removes an unused old copy from memory.

Each colour stays one second: black, red, green, blue, white, light blue,
magenta, then a fade from red to blue. Normally it runs remotely (amictl);
it works by hand too.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-5
       Execute Run-M1
2. Five steps, about a minute. Read the Shell before each window opens.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-5
       Execute Run-M1 FROM=4

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_clear         clears and presents, then checks the pixels
bin/smoke            the M0 smoke test, as a regression check
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
