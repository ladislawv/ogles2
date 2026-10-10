ogles2 M1 run 4
===============

What this is
------------
Runs 1 to 3 showed the GPU path works, but a full-strength colour (255)
comes out as 0, and 191 as 192, with or without dithering. The library's
own number conversion is correct in the program code, so this run measures
each stage separately, on a hidden bitmap (nothing opens on the screen):

  1. the CPU's float to byte conversion;
  2. reading pixels back (bytes the CPU wrote, read with ReadRGBPixel);
  3. the GPU loading a picture and storing it back unchanged;
  4. the GPU clearing to every value 0 to 255 in every channel.

The library logs the clear colour words it sends to the GPU.

It is meant to run remotely with amictl (amictl.py runpkg), but works by
hand too. Please do not run a MiniGL program at the same time. Nothing is
installed: libs/ is added to LIBS: for the run and removed at the end.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-4
       Execute Run-M1
2. Two steps, under a minute, nothing to watch.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step:
       CD <...>/ogles2-m1-4
       Execute Run-M1 FROM=2

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_probe         the colour probe
bin/m1_clear         the run 3 test (bitmap part only here)
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
