ogles2 M3 run 1
===============

What this is
------------
The first run with the shader compiler on the Amiga. Until now every
program needed its shaders compiled on a PC beforehand; this package has no
compiled shaders at all. The library starts the compiler (libs/v3dglslc,
19 MB) the first time a program needs one, which takes a few seconds, and
then compiles each program in a fraction of a second.

First four tests that draw on hidden bitmaps (nothing to see), then es2gears
and ten book samples, each in its own window for two or three seconds; the
Shell says what each should show. About two minutes.

Normally it runs remotely (amictl); it works by hand too. Please do not
run a MiniGL program at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m3-1
       Execute Run-M1
2. Fifteen steps, about two minutes.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot, look at the last line of results/00-progress.txt, and continue
with the next step, for example:
       CD <...>/ogles2-m3-1
       Execute Run-M1 FROM=4

Contents
--------
Run-M1               the test script
libs/ogles2.library  the library (logs every line at once)
libs/v3dglslc        the shader compiler, started by the library
bin/                 the test programs and the eleven corpus programs
shaders/             GLSL sources two tests read
data/                the images two of the samples load
cache/               where the compiled shaders go during the run
notes-template.txt   copied to results/notes.txt at the end
