amictld - the remote test station
=================================

What this is
------------
A small server that lets the development PC (192.168.1.32) send test
programs to this Amiga, run them, and fetch their results, logs and
screenshots over the network, so you no longer have to copy packages and
send results back by hand.

It only accepts connections from the address in amictld.allow, and only
with the secret in amictld.token. Every command it receives is written to
amictld.log in its directory, so you can always see what was done. It
never shows requesters. Port 6800 must not be opened to the internet in
your router.

Install (once)
--------------
1. Copy this whole directory to a permanent place on a hard disk
   partition, for example Stuff:amictld (the rest of this text uses that
   name; put your own in its place - not every Amiga has a Work: volume).
2. Add this line at the end of S:User-Startup (create the file if it does
   not exist), with the place from step 1:
       Run >NIL: Stuff:amictld/amictld
   No space between > and NIL:. It waits for Roadshow by itself, so its
   position does not matter. Keep a copy of S:User-Startup before you
   edit it.
3. Start it now without a reboot:
       CD Stuff:amictld
       Execute Start-amictld
   (or simply reboot).
4. Tell me it is running; I take it from there.

After a crash or a hang, just reset the Amiga: the server starts again by
itself and I continue.

If something is wrong
---------------------
- Look at Stuff:amictld/amictld.log: it records each start, each
  connection (refused ones too) and each command.
- If the PC's address ever changes (192.168.1.32 now), the server refuses
  it: put the new address into amictld.allow, one address per line, and
  restart the server (or reboot). A fixed address for the PC in your
  router avoids this.

Uninstall
---------
Remove the line from S:User-Startup, reboot, and delete Stuff:amictld.

Contents
--------
amictld          the server (needs bsdsocket.library, i.e. Roadshow)
amictld.token    the shared secret (keep it private)
amictld.allow    the PC's address
Start-amictld    starts the server now, without a reboot
ReadMe.txt       this file
