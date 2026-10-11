ogles2 spike run 1
==================

What this is
------------
A timing test of the GLSL shader compiler ogles2 will use. It is Mesa's
compiler (the one Linux uses for the Raspberry Pi's GPU), built for 68k.
On this PC it already produces exactly the same results as the Linux build;
this run measures how fast it is on your A1200 under Emu68.

It only reads the files in this directory and writes into results/.
Nothing is installed and no system file is changed.

The program is large (11 MB), so copy the whole directory to a hard disk
partition, not to RAM:.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-spike-1
       Execute Run-Spike
2. It runs by itself: 15 compiler runs in 6 steps. Each run starts the
   compiler once, so expect some seconds per run; nothing appears on screen
   while a run is busy.
3. At the end, write a few words into results/notes.txt.
4. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, then look at the last line of results/00-progress.txt and continue
with the next step, for example:
       CD <...>/ogles2-spike-1
       Execute Run-Spike FROM=4
Please mention it in results/notes.txt.

Contents
--------
Run-Spike            the test script
bin/glsl_compiler    Mesa's standalone GLSL compiler for 68k (GCC 16.2, libnix)
bin/timeit           runs a command and logs its time (source: timeit.c)
shaders/             the test shaders
notes-template.txt   copied to results/notes.txt at the end
