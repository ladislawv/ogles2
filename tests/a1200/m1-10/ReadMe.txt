ogles2 M1 run 10
================

What this is
------------
The M1 tests together once more, after two changes the probes of runs 8
and 9 led to: the library now takes GPU memory only from fast RAM the GPU
can reach (the device tree's ranges), and 16.16 fixed-point vertex data
goes to the GPU as it is. Three steps: the probes (without the one that
crashed run 8), the triangle test on all three surfaces without pauses,
and the 10,000-frame soak in a window.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-10
       Execute Run-M1
2. Three steps, under a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-10
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_hwprobe       the probes
bin/m1_soak          10,000 frames
bin/m1_draw          the triangle test
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
