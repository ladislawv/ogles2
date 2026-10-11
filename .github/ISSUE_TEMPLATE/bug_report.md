---
name: Bug report
about: Something draws wrongly, crashes or fails to link
labels: bug
---

**What happened, and what you expected**


**The program** (name, where to get it, or a minimal source; the shaders if they fail to link)


**System**
- Amiga model, PiStorm version, Raspberry Pi model (4B / CM4):
- Emu68 version:
- AmigaOS version, RTG system (Picasso96 / CyberGraphX), screen mode and depth:
- ogles2.library version (`Version LIBS:ogles2.library`) or commit:

**The library's log**
Run the program with `SetEnv OGLES2/LogFile RAM:ogles2.log` and attach `RAM:ogles2.log`
(after a crash: run `OGLES2Log` before rebooting if the machine still responds).
If a shader failed to link, attach `T:ogles2_missing_shaders.txt` too.

**Crash requester or SmartCrash/Enforcer output, if any**
