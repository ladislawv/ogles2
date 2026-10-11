ogles2 v3dp run 1
=================

What this is
------------
The same 68k shader compiler as spike run 2, now also writing its output as
GPU program files (.v3dp). These are the files ogles2.library will load: the
GPU code of each shader plus the tables the library needs to set uniforms,
textures and vertex attributes. On this PC the 68k build and the Linux build
write identical files, byte for byte. This run checks that the A1200 writes
them identically too.

The GPU itself is not used. It only reads the files in this directory and
writes into results/. Nothing is installed and no system file is changed.

The program is large (19 MB), so copy the whole directory to a hard disk
partition, not to RAM:.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-v3dp-1
       Execute Run-V3DP
2. One run, a few seconds. Nothing appears on screen while it is busy.
3. Write a few words into results/notes.txt.
4. Send me the whole results directory (with results/v3dp inside).

Contents
--------
Run-V3DP             the test script
bin/v3dc             Mesa's GLSL compiler and V3D driver for 68k, with the .v3dp writer
bin/timeit           runs a command and logs its time
shaders/             the test shaders
notes-template.txt   copied to results/notes.txt at the end
