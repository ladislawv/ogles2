ogles2 M7 run 7
===============

What this is
------------
SDL2 with three fixes from a user's report: sound through AHI (it stopped
after a moment), full screen on the PiStorm's RTG (a 32-bit screen, and
no blank screen left behind afterwards), and no debug log on WORK:.

You hear a quiet tone twice (3 seconds each), see two SDL tests in a
window, then two in full screen: the screen switches to them and back to
the Workbench. The whole run takes about a minute.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m7-7
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line. To go on after the step that hung:   Execute Run-M1 FROM=<the next step>

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
libs/v3dglslc        the shader compiler (SDL writes its shaders at run time)
bin/audiotest        plays a tone and checks the sound keeps going
bin/screens          lists the open screens
bin/testgles2, testsprite2, testrendercopyex, testdraw2   SDL's tests
bin/OGLES2Log        writes the library's log file on request
cache/               compiled shaders
notes-template.txt   copied to results/notes.txt at the end
