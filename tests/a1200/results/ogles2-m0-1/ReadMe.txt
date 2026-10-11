ogles2 M0 run 1
===============

What this is
------------
The first build of ogles2.library itself: the library skeleton, the
program-side library (libogles2.a), the file log, and PiStorm3D's GPU
layer. It does not draw yet. When the first context is created it starts
the GPU the way minigl.library does (power on, read its identity); all GL
functions except glGetError and glGetString are placeholders that report
"not implemented".

Please do not run a MiniGL program at the same time.

This run checks on the real machine that the library loads, opens and
closes, that it finds the GPU and reports its identity (version, number
of shader processors, VPM size), that its function table has the
expected layout, that 20 open/close sessions lose no memory, that it
writes its log file, and that it leaves memory again when asked to
(Avail FLUSH).

Nothing is installed. The script adds this directory's libs/ to LIBS:
for the run and removes it at the end; the library's log goes to
results/ogles2.log through a temporary ENV:OGLES2/LogFile variable.

How to run
----------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m0-1
       Execute Run-M0
2. Four short steps, a few seconds in all.
3. Write a few words into results/notes.txt.
4. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Please reboot and send results/ as it is; 00-progress.txt shows how far
it got. Mention it in results/notes.txt.

Contents
--------
Run-M0               the test script
libs/ogles2.library  the library (GCC 6.5.0b, 68020 + FPU)
bin/smoke            the smoke test
bin/abi_layout       prints the checked function-table layout
bin/OGLES2Log        writes the library's log file on request
notes-template.txt   copied to results/notes.txt at the end
