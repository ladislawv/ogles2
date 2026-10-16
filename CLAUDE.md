# ogles2: notes for Claude

OpenGL ES 2.0 for AmigaOS 3 on the PiStorm's V3D 4.2 GPU (Raspberry Pi 4, Emu68).

**Read first:** [`CONTRIBUTING.md`](CONTRIBUTING.md) (the handbook: build machine, building,
testing on the Amiga, conventions, traps) and [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md)
§13 (status; its "Next" list is the work queue).

## Build machine

Ubuntu 24.04, on Windows inside WSL; everything builds there, the repository may live on a
Windows drive (`/mnt/c/...`), output goes to `~/ogles2-build`; the release archive is built
by `bash release/build_release.sh` (CONTRIBUTING.md, "Building"; `sdl` adds the SDL archive); `... dist` also refreshes the
committed archives in `release/dist/` (only when a release changes: ~10 MB of history each). Set it up with the scripts in
[`dev/host-setup/`](dev/host-setup/README.md) in the order its README gives (the toolchains
take hours). Ports fetch their own sources at pinned commits on first build (gl4es, SDL2/3,
Quake III: `ports/ioq3/sources.sh`). `ogles2` and `ioq3-amiga` are private GitHub
repositories: git in WSL needs credentials (CONTRIBUTING.md, "Access").

## Rules

- When something is done, decided or found, update the plan (the relevant section and §13)
  in the same change; do not write separate documents.
- Commit only when asked; the commit message ends with the agent attribution line.
- Never commit `tools/amictl/station.local` (the test station's token) or screenshots.
- Tests on the A1200 run through `tools/amictl` (`runpkg` a package from
  `tests/a1200/<name>`); visible tests show each state about a second; benchmarks run short.
- Before rebooting the Amiga or changing its system files, ask the user.
- Do not modify sources outside this repository (the Quake III trees are clones made by
  `ports/ioq3/sources.sh`; changes to them are patches in `ports/ioq3*/patches/`).

## Traps that cost time (more in CONTRIBUTING.md)

- PowerShell mangles `$`, `\` and quotes passed to `wsl bash -c`: write a script file and
  run `wsl -d Ubuntu-24.04 bash <script>` (the name `wsl -l` lists; on some machines `Ubuntu`).
- Windows Python writes CRLF; files here are LF (`.gitattributes`). Make patches to the
  ported sources against the clones the builds use (`~/ioq3-upstream`, `~/ioq3-amiga`, in
  WSL), not against a Windows checkout: git on Windows turns LF into CRLF there.
- An ogles2.library already in memory stays there: test scripts run `Avail FLUSH` first.
- An ogles2 installed in the Amiga's `LIBS:` (by the release's `Install-ogles2`) wins over a
  package's `libs/` (added after it): check `List LIBS:ogles2#? LIBS:v3dglslc` before tests.
  The station cannot answer the installer's questions: `Execute Install-ogles2 ALL`.
- Quake III keeps settings in `home/baseq3/q3config.cfg` between runs: delete it for a clean
  comparison. A timedemo's sound is garbled by nature (it runs faster than real time).
