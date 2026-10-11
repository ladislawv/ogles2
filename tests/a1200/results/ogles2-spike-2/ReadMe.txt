ogles2 spike run 2
==================

What this is
------------
The complete shader compiler ogles2 will use: Mesa's GLSL compiler together
with Mesa's driver for the Pi 4's GPU, built for 68k. It turns GLSL shaders
into the GPU's own machine code, exactly as Linux does on a Raspberry Pi.
On this PC its output is identical to the Linux build. This run measures
how fast it is on your A1200, and checks that Emu68 gives the same results.

The GPU itself is not used: the compiler only produces code.
It only reads the files in this directory and writes into results/.
Nothing is installed and no system file is changed.

The program is large (19 MB), so copy the whole directory to a hard disk
partition, not to RAM:.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-spike-2
       Execute Run-Spike2
2. It runs by itself: 16 runs in 6 steps. Nothing appears on screen while
   a run is busy.
3. At the end, write a few words into results/notes.txt.
4. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, then look at the last line of results/00-progress.txt and continue
with the next step, for example:
       CD <...>/ogles2-spike-2
       Execute Run-Spike2 FROM=4
Please mention it in results/notes.txt.

Contents
--------
Run-Spike2           the test script
bin/v3dc             Mesa's GLSL compiler and V3D driver for 68k (GCC 16.2, software floating point)
bin/timeit           runs a command and logs its time
shaders/             the test shaders
notes-template.txt   copied to results/notes.txt at the end
