ogles2 M7 run 8
===============

What this is
------------
A fullscreen SDL window is a window that covers a screen of its own. The
library now draws such a window straight into its screen, as it draws a
screen of its own, instead of into a hidden picture that is copied into
the window at every frame. The frame is shown without the copy, and the
program goes on with its next frame while the graphics chip draws this one.

Step 1 checks the colours shown on such a screen (the test reads them back
itself), next to a normal window and a screen of the library's own. Steps
3 to 7 run SDL's tests full screen; step 4 runs one with the old copy, for
the comparison of their speed.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time. About 1 minute 30 seconds.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m7-8
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line. To go on after the step that hung:   Execute Run-M1 FROM=<the next step>

Contents
--------
  Run-M1               the script
  libs/ogles2.library  the library (it writes every log line at once)
  libs/v3dglslc        the shader compiler (SDL writes its shaders at run time)
  bin/                 m1_clear, SDL2's tests, screens, OGLES2Log
  bin3/                SDL3's tests
  cache/               compiled shaders, made by the run
