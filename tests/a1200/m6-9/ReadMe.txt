ogles2 M6 run 9
===============

What this is
------------
A look at how the screen shows a moving picture, before ogles2 changes
how it presents frames. A white bar crosses a 640x480 screen four
different ways, 6 seconds each (about half a minute in all). The white
squares at the top of the screen give the way's number, 1 to 4. There is
no 3D here.

Please watch the screen. For each of the four ways, note whether the bar
moves smoothly, moves jerkily (stops and jumps), or looks torn (broken
into pieces that do not line up).

The four ways, in order:
  1 drawn straight onto the visible screen, as fast as possible
  2 drawn straight onto the visible screen, once per screen refresh
  3 drawn on a hidden screen buffer, then switched to it
  4 the same, switching only once per screen refresh

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m6-9
       Execute Run-M1
2. Fill in results/notes.txt (one line per way).
3. Send me the whole results directory.

Contents
--------
Run-M1               the test script
bin/m6_flip          the test
notes-template.txt   copied to results/notes.txt at the end
