ogles2 M1 run 9
===============

What this is
------------
The last part of M1. First four hardware probes, each tried once on a
hidden bitmap and compared with the normal path:
  F  can the GPU read 16.16 fixed-point vertex data directly?
  Y  (run 8 answered it: it crashed the machine; not run again)
  S  can the GPU start rendering by itself as soon as binning ends?
  A  can the GPU read vertex data from Zorro II RAM and from the top of
     the Pi's memory? (The library also lists every memory region.)
The last one tried (S) could hang the GPU; if the machine stops, reset it
and continue with step 2.

Then the soak: 10,000 frames in a window, about half a minute, checking
that nothing leaks and no frame is lost. Then the step 2 test again.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-9
       Execute Run-M1
2. Three steps, about a minute.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-9
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_hwprobe       the four probes
bin/m1_soak          10,000 frames
bin/m1_draw          the step 2 test, as a regression check
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
