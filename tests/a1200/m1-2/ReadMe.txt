ogles2 M1 run 2
===============

What this is
------------
Run 1 worked: the GPU cleared and showed every picture, with no dropped
frame. But every colour channel at full strength came out as 0, because
of a compiler bug in converting fractions to whole numbers (now worked
around). Run 2 repeats the test with that fixed; each colour now stays
on screen for a moment, so you can see it.

The first time ogles2.library uses the GPU to produce pixels: it clears
the picture to a colour on the GPU and shows it, in the four places a
program can draw into:
  - an application's bitmap (nothing is shown; the program checks the
    pixels itself),
  - a window ogles2 opens itself,
  - a window the program opens,
  - a fullscreen screen.
After every frame the test reads back what is actually on screen and
compares it with the colour it asked for, so it judges itself; your eyes
are a second check. It also times 100 frames and checks that closing a
picture gives all its memory back.

This uses the GPU the way minigl.library does. Please do not run a MiniGL
program at the same time.

Nothing is installed. libs/ is added to LIBS: for the run and removed at
the end; the library's log goes to results/ogles2.log (this build writes
every log line at once, so a hang loses nothing).

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m1-2
       Execute Run-M1
2. Five steps, about half a minute. Windows and a full screen flash colours.
3. Write a few words into results/notes.txt.
4. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m1-2
       Execute Run-M1 FROM=4
Please mention it in results/notes.txt; results/ogles2.log shows how far
the library got.

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
bin/m1_clear         clears and presents, then checks the pixels
bin/smoke            the M0 smoke test, as a regression check
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
