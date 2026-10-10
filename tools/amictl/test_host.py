#!/usr/bin/env python3
"""Tests amictld (host build) and the amictl client on this PC, before
anything reaches the A1200 (plan section 10.1, step 1).
  usage: test_host.py SERVER-BINARY
Starts the server in a temporary root, runs every command through the
client, checks the answers, and stops it. Exit status 0 when all pass.
"""
import os
import re, shutil, socket, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import amictl

PORT = 6800
TOKEN = "0123456789abcdef0123456789abcdef"
failures = 0
checks = 0


VERSION = re.search(r'AMICTL_VERSION\s+"([^"]+)"',
                    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "server", "amictl_proto.h")).read()).group(1)

def check(ok, what):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
    print("%s: %s" % ("PASS" if ok else "FAIL", what))


def start_server(binary, root, allow="127.0.0.1"):
    os.makedirs(os.path.join(root, "base"), exist_ok=True)
    open(os.path.join(root, "base", "amictld.token"), "w").write(TOKEN + "\n")
    open(os.path.join(root, "base", "amictld.allow"), "w").write(allow + "\n")
    env = dict(os.environ, AMICTLD_ROOT=root)
    p = subprocess.Popen([binary], env=env, stderr=open(os.path.join(root, "server.err"), "a"))
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            return p
        except OSError:
            time.sleep(0.1)
    return p


def station():
    return amictl.Station("127.0.0.1", PORT, TOKEN)


def raw_request(cmd, arg=b"", data=b"", magic=b"AMC1", alen=None):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(magic + struct.pack(">III", cmd, len(arg) if alen is None else alen, len(data)) +
              arg + data)
    try:
        head = s.recv(12)
    except OSError:
        head = b""
    s.close()
    return head


def main():
    binary = os.path.abspath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="amictld-test-")
    srv = start_server(binary, root)
    try:
        # authentication
        head = raw_request(1, b"wrong-token-wrong-token")
        check(head[:4] == b"AMC1" and struct.unpack(">I", head[4:8])[0] == 2,
              "wrong token is refused (status 2)")
        head = raw_request(2)
        check(struct.unpack(">I", head[4:8])[0] == 2 if len(head) >= 8 else False,
              "a command before HELLO is refused")
        head = raw_request(1, b"x", magic=b"XXXX")
        check(head == b"", "bad magic closes the connection")
        head = raw_request(1, b"", alen=100000)
        check(len(head) >= 8 and struct.unpack(">I", head[4:8])[0] == 9,
              "oversized argument is refused (status 9)")

        st = station()
        check(st.hello_text.startswith("amictld " + VERSION) and "starts 1" in st.hello_text,
              "HELLO: version and start count 1 (%s)" % st.hello_text.replace("\n", " "))
        check("protocol test build" in st.info(), "INFO")

        # files
        blob = os.urandom(1 << 20)
        st.call("MAKEDIR", "WORK:t")
        st.put("WORK:t/blob.bin", blob)
        check(st.get("WORK:t/blob.bin") == blob, "PUT/GET 1 MB round trip")
        names = [n for k, s, n in st.listdir("WORK:t")]
        check(names == ["blob.bin"], "LIST shows the file and no .tmp left over (%s)" % names)
        st.call("DELETE", "WORK:t/blob.bin")
        try:
            st.get("WORK:t/blob.bin")
            check(False, "GET of a deleted file fails")
        except amictl.StationError as e:
            check(e.status == 3, "GET of a deleted file: not found")

        # jobs
        rc, out, to = st.run("echo hello; echo err >&2; exit 3", timeout=10)
        check(rc == 3 and b"hello" in out and not to, "RUN: exit code 3 and output")
        t0 = time.time()
        rc, out, to = st.run("sleep 30", timeout=1, grace=5)
        check(to and rc is not None and time.time() - t0 < 10,
              "RUN timeout: Ctrl-C ended it (rc %s, %.1f s)" % (rc, time.time() - t0))
        job = st.start("sleep 1; echo late")
        done, rc, out = st.wait(job, 0.2)
        check(not done, "START/WAIT: not done after 0.2 s")
        done, rc, out = st.wait(job, 5)
        check(done and rc == 0 and out.strip() == b"late", "START/WAIT: done, output")
        try:
            st.wait(job, 1)
            check(False, "WAIT on a collected job fails")
        except amictl.StationError as e:
            check(e.status == 8, "WAIT on a collected job: no such job")
        rc, out, to = st.run("pwd", directory="WORK:t", timeout=5)
        check(out.strip().endswith(b"/WORK/t"), "RUN in a directory (%s)" % out.strip().decode())

        # screenshot
        w, h, rgb = st.shot()
        png = os.path.join(root, "shot.png")
        amictl.write_png(png, w, h, rgb)
        check((w, h, len(rgb)) == (4, 3, 36) and open(png, "rb").read(8) == b"\x89PNG\r\n\x1a\n",
              "SHOT: 4x3 test image written as PNG")

        # environment
        st.call("SETENV", "OGLES2/StoreDither", b"1")
        check(st.call("GETENV", "OGLES2/StoreDither") == b"1", "SETENV/GETENV")
        st.call("UNSETENV", "OGLES2/StoreDither")
        try:
            st.call("GETENV", "OGLES2/StoreDither")
            check(False, "UNSETENV")
        except amictl.StationError:
            check(True, "UNSETENV")

        # self-update: upload the same binary as amictld.new and restart into it
        st.put("PROGDIR:amictld.new", open(binary, "rb").read())
        st.call("RESTART", "amictld.new")
        st.close()
        time.sleep(1)
        st = None
        for _ in range(50):
            try:
                st = station()
                break
            except OSError:
                time.sleep(0.2)
        check(st is not None and "starts 2" in st.hello_text,
              "RESTART: the new server answers, start count 2")
        st.call("QUIT")
        st.close()
        time.sleep(0.5)
        try:
            station()
            check(False, "QUIT stops the server")
        except OSError:
            check(True, "QUIT stops the server")

        # an address that is not allowed
        srv2 = start_server(binary, root, allow="10.9.8.7")
        head = raw_request(1, TOKEN.encode())
        check(head == b"", "a client not in amictld.allow is refused before reading")
        srv2.terminate()
        srv2.wait()
        log = open(os.path.join(root, "base", "amictld.log")).read()
        check("not in amictld.allow" in log and "authentication failed" in log and
              "RUN" not in log and "START" in log, "the log records refusals and commands")
    finally:
        srv.terminate()
        try:
            srv.wait(5)
        except subprocess.TimeoutExpired:
            srv.kill()
        subprocess.run(["pkill", "-f", os.path.join(root, "base", "amictld.new")], check=False)
        shutil.rmtree(root, ignore_errors=True)
    print("%s: %d of %d checks passed" % ("ALL PASSED" if not failures else "FAILED",
                                          checks - failures, checks))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
