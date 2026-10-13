#!/usr/bin/env python3
"""amictl: client of the remote test station (plan section 10.1).

Talks to amictld on the A1200. The station address and token come from
tools/amictl/station.local (JSON: {"host": ..., "port": ..., "token": ...},
not in git) or from AMICTL_HOST / AMICTL_PORT / AMICTL_TOKEN.

  amictl hello | info
  amictl put LOCAL REMOTE          amictl put -r LOCALDIR REMOTEDIR
  amictl get REMOTE LOCAL          amictl get -r REMOTEDIR LOCALDIR
  amictl ls DIR | rm PATH | mkdir DIR
  amictl run [-t SECONDS] [-d DIR] COMMAND...    (exit status = the command's)
  amictl start [-d DIR] COMMAND... | wait JOB [SECONDS] | abort JOB
  amictl shot OUT.png [SCREEN-TITLE-PART]
  amictl getenv NAME | setenv NAME VALUE | unsetenv NAME
  amictl up [SECONDS]              wait until the station answers (after a reboot)
  amictl reboot | restart NEWBINARY | quit
  amictl runpkg PACKAGEDIR [-t SECONDS] [--shots SECONDS] [-a "KEY=VALUE ..."]
                                   upload, run its Run-* script, fetch results/;
                                   --shots: a screenshot every SECONDS meanwhile
  amictl gentoken FILE             write a new random token

MIT licence (tools/amictl/LICENSE).
"""
import datetime, json, os, re, secrets, socket, struct, sys, time, zlib

MAGIC = b"AMC1"
CMD = {"HELLO": 1, "INFO": 2, "PUT": 3, "GET": 4, "LIST": 5, "DELETE": 6, "MAKEDIR": 7,
       "START": 8, "WAIT": 9, "ABORT": 10, "SHOT": 11, "GETENV": 12, "SETENV": 13,
       "UNSETENV": 14, "REBOOT": 15, "RESTART": 16, "QUIT": 17}
STATUS = {0: "ok", 1: "bad request", 2: "not authenticated", 3: "not found", 4: "I/O error",
          5: "out of memory", 6: "busy", 7: "unsupported", 8: "no such job", 9: "too big"}
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


class StationError(Exception):
    def __init__(self, status, what):
        super().__init__("%s: %s" % (what, STATUS.get(status, "status %d" % status)))
        self.status = status


def config():
    cfg = {"host": None, "port": 6800, "token": None}
    p = os.path.join(HERE, "station.local")
    if os.path.exists(p):
        cfg.update(json.load(open(p)))
    cfg["host"] = os.environ.get("AMICTL_HOST", cfg["host"])
    cfg["port"] = int(os.environ.get("AMICTL_PORT", cfg["port"]))
    cfg["token"] = os.environ.get("AMICTL_TOKEN", cfg["token"])
    if not cfg["host"]:
        sys.exit("amictl: no station address (tools/amictl/station.local, see "
                 "station.local.example, or AMICTL_HOST)")
    if not cfg["token"]:
        sys.exit("amictl: no token (tools/amictl/station.local or AMICTL_TOKEN)")
    return cfg


class Station:
    def __init__(self, host=None, port=None, token=None, timeout=30):
        cfg = config() if token is None else {"host": host, "port": port, "token": token}
        self.addr = (host or cfg["host"], port or cfg["port"])
        self.sock = socket.create_connection(self.addr, timeout=timeout)
        self.hello_text = self.call("HELLO", cfg["token"]).decode("latin-1")

    def close(self):
        self.sock.close()

    def _recv(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.sock.recv(min(n - len(out), 1 << 20))
            if not chunk:
                raise ConnectionError("station closed the connection")
            out += chunk
        return bytes(out)

    def call(self, cmd, arg="", data=b"", timeout=None):
        a = arg.encode("latin-1")
        self.sock.sendall(MAGIC + struct.pack(">III", CMD[cmd], len(a), len(data)) + a + data)
        old = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            head = self._recv(12)
            if head[:4] != MAGIC:
                raise ConnectionError("bad reply")
            status, n = struct.unpack(">II", head[4:])
            body = self._recv(n)
        finally:
            self.sock.settimeout(old)
        if status:
            raise StationError(status, "%s %s" % (cmd, arg.replace("\n", " ")))
        return body

    # ---- conveniences ----
    def info(self):
        return self.call("INFO").decode("latin-1")

    def put(self, remote, data):
        self.call("PUT", remote, data, timeout=300)

    def get(self, remote):
        return self.call("GET", remote, timeout=300)

    def listdir(self, d):
        out = []
        for line in self.call("LIST", d).decode("latin-1").splitlines():
            kind, rest = line.split(" ", 1)
            if kind == "D":
                out.append(("D", 0, rest))
            else:
                size, name = rest.split(" ", 1)
                out.append(("F", int(size), name))
        return out

    def start(self, command, directory=""):
        return struct.unpack(">I", self.call("START", "%s\n%s" % (directory, command)))[0]

    def wait(self, job, seconds):
        body = self.call("WAIT", "%d\n%d" % (job, int(seconds * 1000)), timeout=seconds + 30)
        done, rc = struct.unpack(">Ii", body[:8])
        return bool(done), rc, body[8:]

    def run(self, command, directory="", timeout=60, grace=10):
        """Returns (rc, output, timed_out). On timeout the command gets Ctrl-C."""
        job = self.start(command, directory)
        done, rc, out = self.wait(job, timeout)
        if done:
            return rc, out, False
        self.call("ABORT", str(job))
        done, rc, out = self.wait(job, grace)
        if not done:
            return None, b"(still running after Ctrl-C: left alone, job %d)\n" % job, True
        return rc, out, True

    def shot(self, screen="", timeout=120):
        body = self.call("SHOT", screen, timeout=timeout)
        w, h = struct.unpack(">II", body[:8])
        return w, h, body[8:]


def join(remote_dir, name):
    if not remote_dir or remote_dir.endswith((":", "/")):
        return remote_dir + name
    return remote_dir + "/" + name


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def put_tree(st, local, remote):
    try:
        st.call("MAKEDIR", remote)
    except StationError:
        pass
    n = 0
    for name in sorted(os.listdir(local)):
        lp = os.path.join(local, name)
        if os.path.isdir(lp):
            n += put_tree(st, lp, join(remote, name))
        else:
            st.put(join(remote, name), open(lp, "rb").read())
            n += 1
    return n


def get_tree(st, remote, local):
    os.makedirs(local, exist_ok=True)
    n = 0
    for kind, size, name in st.listdir(remote):
        if kind == "D":
            n += get_tree(st, join(remote, name), os.path.join(local, name))
        else:
            open(os.path.join(local, name), "wb").write(st.get(join(remote, name)))
            n += 1
    return n


def up(seconds):
    end = time.time() + seconds
    while True:
        try:
            st = Station(timeout=5)
            return st
        except (OSError, ConnectionError) as e:
            if time.time() > end:
                raise SystemExit("amictl: station not answering after %d s (%s)" % (seconds, e))
            time.sleep(3)


def run_with_shots(st, command, directory, timeout, every, shotdir):
    """Like Station.run, taking a screenshot about every `every` seconds while
    the command runs. The server serves one connection at a time, so the
    screenshots and the waiting share this one."""
    os.makedirs(shotdir, exist_ok=True)
    job = st.start(command, directory)
    t0 = time.time()
    n = 0
    out = b""
    while True:
        try:
            w, h, rgb = st.shot(timeout=40)
        except (OSError, ConnectionError) as e:
            # M1 run 5: one reply stalled in the network while a screen
            # closed. The job runs on regardless; reconnect and go on.
            print("amictl: screenshot failed (%s), reconnecting" % e)
            st.close()
            st.__init__()
            rgb = None
        if rgb is not None:
            step = ""
            for line in reversed(out.decode("latin-1", "replace").splitlines()):
                if line.startswith("Step "):
                    step = "-step" + line.split()[1]
                    break
            write_png(os.path.join(shotdir, "%03d-%05.1fs%s.png" % (n, time.time() - t0, step)),
                      w, h, rgb)
            n += 1
        done, rc, out = st.wait(job, every)
        if done:
            return rc, out, False, n
        if time.time() - t0 > timeout:
            st.call("ABORT", str(job))
            done, rc, out = st.wait(job, 10)
            return (rc if done else None), out, True, n


def runpkg(st, pkgdir, timeout, shots=0, script_args=""):
    """Upload a package (tests/a1200/<name>/ built into tests/a1200/out/ogles2-<name>/),
    run its Run-* script, fetch results/ into tests/a1200/results/<package>-<time>/.
    With shots, a screenshot about every `shots` seconds goes into shots/ there.
    script_args (KEY=VALUE words) go to the script's .key line; a value may hold
    "-" and "*" too (dEQP's test patterns), never quotes, spaces or ";"."""
    if script_args and not re.fullmatch(r"[A-Za-z0-9_]+=[A-Za-z0-9_.*-]+( [A-Za-z0-9_]+=[A-Za-z0-9_.*-]+)*",
                                        script_args):
        raise SystemExit("amictl: script arguments must be KEY=VALUE words: %r" % script_args)
    pkgdir = os.path.abspath(pkgdir)
    name = os.path.basename(pkgdir.rstrip("/\\"))
    scripts = [f for f in os.listdir(pkgdir) if f.startswith("Run-")]
    if len(scripts) != 1:
        raise SystemExit("amictl: %s needs exactly one Run-* script" % pkgdir)
    # Packages go into the server's own work directory (PROGDIR:work, wherever
    # the server is installed); the Amiga may have no Work: volume.
    rc, out, _ = st.run("CD", "", 30)
    workdir = out.decode("latin-1").strip().splitlines()[-1] if rc == 0 and out.strip() else ""
    if ":" not in workdir:
        raise SystemExit("amictl: cannot find the server's work directory (CD said %r)" % out)
    remote = join(workdir, name)
    print("uploading %s -> %s: %d files" % (pkgdir, remote, put_tree(st, pkgdir, remote)))
    # Results of an earlier run of the same package would mix with these.
    st.run('Delete "%s" ALL QUIET FORCE >NIL:' % join(remote, "results"), "", 60)
    # A library left in memory by an earlier run would be used instead of the
    # package's (M1 runs 2 and 3 ran run 1's library this way): expunge every
    # library nobody has open.
    st.run("Avail FLUSH >NIL:", "", 60)
    dest = os.path.join(REPO, "tests", "a1200", "results",
                        "%s-%s" % (name, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    t0 = time.time()
    if shots:
        rc, out, timed_out, n = run_with_shots(st, ("Execute %s %s" % (scripts[0], script_args)).strip(),
                                               remote, timeout,
                                               shots, os.path.join(dest, "shots"))
        print("%d screenshots -> %s" % (n, os.path.join(dest, "shots")))
    else:
        rc, out, timed_out = st.run(("Execute %s %s" % (scripts[0], script_args)).strip(), remote, timeout)
    print(out.decode("latin-1", "replace"), end="")
    print("%s: rc=%s%s, %.1f s" % (scripts[0], rc, " (TIMED OUT)" if timed_out else "",
                                   time.time() - t0))
    n = get_tree(st, remote + "/results", os.path.join(dest, "results"))
    print("results: %d files -> %s" % (n, dest))
    log = os.path.join(dest, "results", "ogles2.log")
    if os.path.exists(log):
        head = open(log, "rb").read(4096).decode("latin-1", "replace").splitlines()[:3]
        if not any(" init" in line for line in head):
            print("WARNING: ogles2.log does not start with the library's init line: the "
                  "package's library may not be the one that ran")
    return rc


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "gentoken":
        open(args[0], "w").write(secrets.token_hex(16) + "\n")
        return 0
    if cmd == "up":
        st = up(int(args[0]) if args else 300)
        print(st.hello_text, end="")
        return 0
    st = Station()
    try:
        if cmd == "hello":
            print(st.hello_text, end="")
        elif cmd == "info":
            print(st.info(), end="")
        elif cmd == "put":
            if args[0] == "-r":
                print("%d files" % put_tree(st, args[1], args[2]))
            else:
                st.put(args[1], open(args[0], "rb").read())
        elif cmd == "get":
            if args[0] == "-r":
                print("%d files" % get_tree(st, args[1], args[2]))
            else:
                open(args[1], "wb").write(st.get(args[0]))
        elif cmd == "ls":
            for kind, size, name in st.listdir(args[0] if args else ""):
                print("%s %10s %s" % (kind, size if kind == "F" else "", name))
        elif cmd == "rm":
            st.call("DELETE", args[0])
        elif cmd == "mkdir":
            st.call("MAKEDIR", args[0])
        elif cmd in ("run", "start"):
            timeout, directory = 60, ""
            while args and args[0] in ("-t", "-d"):
                if args[0] == "-t":
                    timeout = float(args[1])
                else:
                    directory = args[1]
                args = args[2:]
            command = " ".join(args)
            if cmd == "start":
                print(st.start(command, directory))
                return 0
            rc, out, timed_out = st.run(command, directory, timeout)
            sys.stdout.write(out.decode("latin-1", "replace"))
            if timed_out:
                print("amictl: timed out after %g s" % timeout, file=sys.stderr)
            return rc if isinstance(rc, int) and 0 <= rc < 256 else 1
        elif cmd == "wait":
            done, rc, out = st.wait(int(args[0]), float(args[1]) if len(args) > 1 else 60)
            sys.stdout.write(out.decode("latin-1", "replace"))
            print("job %s: %s" % (args[0], "rc=%d" % rc if done else "still running"))
        elif cmd == "abort":
            st.call("ABORT", args[0])
        elif cmd == "shot":
            w, h, rgb = st.shot(args[1] if len(args) > 1 else "")
            write_png(args[0], w, h, rgb)
            print("%dx%d -> %s" % (w, h, args[0]))
        elif cmd == "getenv":
            print(st.call("GETENV", args[0]).decode("latin-1"))
        elif cmd == "setenv":
            st.call("SETENV", args[0], " ".join(args[1:]).encode("latin-1"))
        elif cmd == "unsetenv":
            st.call("UNSETENV", args[0])
        elif cmd in ("reboot", "quit"):
            st.call(cmd.upper())
        elif cmd == "restart":
            st.call("RESTART", args[0])
        elif cmd == "runpkg":
            timeout = 600
            if "-t" in args:
                timeout = float(args[args.index("-t") + 1])
            shots = float(args[args.index("--shots") + 1]) if "--shots" in args else 0
            script_args = args[args.index("-a") + 1] if "-a" in args else ""
            rc = runpkg(st, args[0], timeout, shots, script_args)
            return rc if isinstance(rc, int) and 0 <= rc < 256 else 1
        else:
            print("amictl: unknown command %s" % cmd, file=sys.stderr)
            return 2
    except StationError as e:
        print("amictl: %s" % e, file=sys.stderr)
        return 3
    finally:
        st.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
