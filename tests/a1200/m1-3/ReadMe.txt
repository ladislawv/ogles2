ogles2 M1 run 3
===============

What this is
------------
The GPU clears the picture to a colour and shows it, in the four places a
program can draw into: a hidden bitmap, a window ogles2 opens, a window the
program opens, and the whole screen. After every frame the test reads back
what is shown and checks it.

Runs 1 and 2 showed the GPU path works (no dropped frame, all memory given
back), but full-strength colours came out black. Run 2 found why: the GPU
was asked to dither the picture, which on this hardware turns 255 into 0.
ogles2 no longer asks for that. Step 6 asks for it once more on purpose, as
a probe, to confirm the cause; its checks are expected to fail.

This run is slow enough to watch: each colour stays one second, and the
Shell says what to look for before each window or screen opens.

Please do not run a MiniGL program at the same time. Nothing is installed:
libs/ is added to LIBS: for the run and removed at the end; the library's
log goes to results/ogles2.log, every line at once.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-3
       Execute Run-M1
2. Six steps, about a minute. Read the Shell before each window opens.
3. Write a few words into results/notes.txt: what you saw.
4. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-3
       Execute Run-M1 FROM=4
Please mention it in results/notes.txt.

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_clear         clears and presents, then checks the pixels
bin/smoke            the M0 smoke test, as a regression check
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
