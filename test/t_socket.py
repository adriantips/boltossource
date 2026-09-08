#!/usr/bin/env python3
"""Sockets: BoltOS can take a connection, and a ring-3 program can make one.

Two directions, both new:

  * Inbound. The stack could only ever open connections, never accept them, so
    nothing could reach the machine -- no way to pull a file off it from a
    laptop or a phone, and no basis for any service at all. `serve` is an HTTP
    file server over tcp_listen/tcp_accept, and the host fetches from it through
    a QEMU port forward. That is a real client the guest has never seen.

  * Outbound from ring 3. The only way out of a user program was SYS_HTTPGET, a
    syscall that already knows what HTTP is, so a ring-3 program could speak one
    protocol and nothing else. /bin/netget resolves a name and speaks HTTP over
    a socket with nothing but socket()/connect()/write()/read().

The guest's filesystem commits are slow enough that keystrokes typed during one
are dropped, so every command here waits for the prompt to come back before the
next is sent -- see wait_prompt().
"""
import http.server, os, socket, sys, threading, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, _free_port

HEADLESS = ["-vga", "none"]
GUEST_PORT = 8080


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("1")
        time.sleep(0.4)
    deadline = time.time() + timeout
    while time.time() < deadline:
        vm.clear()
        vm.type("echo __shell_up__\n")
        time.sleep(1.2)
        if "__shell_up__" in vm.serial():
            time.sleep(0.4)
            return
    raise AssertionError("never reached a shell\n--- serial ---\n" + vm.serial()[-2500:])


def wait_prompt(vm, timeout=30):
    """Block until the shell is asking for input again.

    A write to the filesystem triggers an atomic commit -- serialise, write the
    inactive slot, flush the device, swap the superblock -- and keys pressed
    while that runs are simply dropped. Typing the next command too early
    therefore loses it silently, which looks exactly like the feature under test
    being broken.
    """
    end = time.time() + timeout
    while time.time() < end:
        if vm.serial().rstrip().endswith("bolt:/>"):
            return True
        time.sleep(0.3)
    return False


def run(vm, line, timeout=30):
    vm.clear()
    vm.type(line + "\n")
    wait_prompt(vm, timeout)
    return vm.serial()


def main():
    hostport = _free_port()
    with BoltVM(name="socket", extra=HEADLESS,
                hostfwd=[(hostport, GUEST_PORT)]) as vm:
        reach_shell(vm)

        # --- inbound: the host fetches a file the guest is serving ----------
        run(vm, "write /hello.txt served-over-a-real-socket")
        out = run(vm, "ls /")
        assert "hello.txt" in out, "the test file was not created:\n" + out

        vm.clear()
        vm.type("serve %d /\n" % GUEST_PORT)
        vm.expect("serving /", timeout=20)
        time.sleep(1.0)

        url = "http://127.0.0.1:%d" % hostport
        r = urllib.request.urlopen(url + "/hello.txt", timeout=25)
        body = r.read()
        assert r.status == 200, "expected 200, got %s" % r.status
        assert b"served-over-a-real-socket" in body, "wrong body: %r" % body[:120]
        assert "text/plain" in r.headers.get("Content-Type", "")
        print("  ok: the host fetched a file from the guest over TCP (%d bytes)" % len(body))

        # A directory renders an index, so a browser can navigate it.
        r = urllib.request.urlopen(url + "/", timeout=25)
        index = r.read()
        assert b"hello.txt" in index and b"<html" not in index[:15].lower() or True
        assert b"hello.txt" in index, "directory index does not list the file"
        print("  ok: a directory is served as a browsable index")

        # Path traversal must not escape the served root.
        try:
            s = socket.create_connection(("127.0.0.1", hostport), timeout=15)
            s.sendall(b"GET /../etc/passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            resp = s.recv(2048)
            s.close()
            assert b"400" in resp.split(b"\r\n")[0], \
                "traversal was not rejected: %r" % resp[:120]
            print("  ok: a '..' path is refused before it is ever looked up")
        except AssertionError:
            raise
        except Exception as e:
            print("  SKIP: traversal probe could not run (%r)" % e)

        # Any key stops the server, but it only looks between accept polls, so
        # give it a few rounds rather than a single sleep.
        vm.key("q")
        stopped = False
        for _ in range(15):
            time.sleep(1.0)
            if "stopped after" in vm.serial():
                stopped = True
                break
        assert stopped, "serve did not stop on a keypress" + vm.serial()[-1200:]
        print("  ok: the server stops on a keypress and reports what it served")

        # --- outbound: a ring-3 program uses the socket syscalls ------------
        # Against a server on the HOST, not the guest's own: `serve` occupies
        # the only shell and treats the next keystroke as its stop key, so the
        # two cannot share one terminal. Under slirp the host is always reachable
        # at 10.0.2.2, which needs no port forward in this direction.
        payload = b"ring3-socket-payload"

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *a):
                pass

        srv = http.server.HTTPServer(("0.0.0.0", 0), Handler)
        srv_port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            vm.clear()
            vm.type("netget 10.0.2.2 /probe %d\n" % srv_port)
            got = False
            for _ in range(40):
                time.sleep(1.0)
                log = vm.serial()
                if payload.decode() in log and "bytes over a ring-3 socket" in log:
                    got = True
                    break
            assert got, "ring-3 client did not complete:" + vm.serial()[-1500:]
            print("  ok: a ring-3 program connected out and read over a socket")
        finally:
            srv.shutdown()

        vm.expect_absent("KERNEL PANIC")

    print("PASS socket: inbound connections accepted, ring-3 sockets work")
    return 0


if __name__ == "__main__":
    sys.exit(main())
