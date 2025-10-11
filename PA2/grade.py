#!/usr/bin/env python3
import argparse, os, sys, time, subprocess, pty, select, signal, hashlib, shutil

FOO = ["foo1", "foo2", "foo3"]

# ---------- build & server ----------
def run(cmd, timeout=300, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)

def make_project():
    print("[*] make clean …")
    try: run(["make", "clean"], timeout=120)
    except Exception: pass
    print("[*] make …")
    r = run(["make"], timeout=300)
    if r.stdout.strip(): print(r.stdout, end="")
    if r.returncode != 0:
        if r.stderr.strip(): print(r.stderr, file=sys.stderr)
        print("[X] Build failed."); return False
    return True

def start_server(port: int) -> subprocess.Popen:
    if not os.path.isfile("server/udp_server"):
        raise FileNotFoundError("server/udp_server not found (build failed?)")
    print(f"[*] starting server: ./udp_server {port} (cwd=server)")
    p = subprocess.Popen(["./udp_server", str(port)],
                         cwd="server",
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.6)  # allow bind
    return p

def stop_proc(p: subprocess.Popen):
    if not p: return
    try:
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try: p.wait(timeout=0.8)
            except subprocess.TimeoutExpired:
                p.terminate()
                try: p.wait(timeout=0.8)
                except subprocess.TimeoutExpired:
                    p.kill()
    except Exception:
        pass

# ---------- single PTY client session ----------
def start_client_pty(host: str, port: int):
    if not os.path.isfile("client/udp_client"):
        raise FileNotFoundError("client/udp_client not found (build failed?)")
    print(f"[*] starting client (PTY): ./udp_client {host} {port} (cwd=client)")
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        ["./udp_client", host, str(port)],
        stdin=slave, stdout=slave, stderr=slave,
        close_fds=True, cwd="client"
    )
    os.close(slave)  # parent keeps only master
    time.sleep(0.2)     # let client prompt
    drain_pty(master, 0.2)  # clear initial prompt/noise
    return master, proc

def stop_client_pty(master_fd: int, proc: subprocess.Popen):
    try:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try: proc.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try: proc.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    except Exception:
        pass
    try: os.close(master_fd)
    except Exception: pass

def drain_pty(master_fd: int, duration: float = 0.0):
    end = time.time() + duration
    while time.time() < end:
        r, _, _ = select.select([master_fd], [], [], 0.05)
        if master_fd in r:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk:
                break

def send_and_capture(master_fd: int, line: str, read_timeout: float = 3.0) -> str:
    print(f"[CMD] {line}")
    drain_pty(master_fd, 0.05)
    if not line.endswith("\n"): line += "\n"
    os.write(master_fd, line.encode("utf-8", "ignore"))
    end = time.time() + read_timeout
    buf = bytearray()
    while time.time() < end:
        r, _, _ = select.select([master_fd], [], [], 0.1)
        if master_fd in r:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk: break
            buf.extend(chunk)
    return buf.decode("utf-8", "ignore")

def send_and_wait_silent(master_fd: int, line: str, wait_secs: float):
    print(f"[CMD] {line}  (silent)")
    drain_pty(master_fd, 0.05)
    if not line.endswith("\n"): line += "\n"
    os.write(master_fd, line.encode("utf-8", "ignore"))
    drain_pty(master_fd, wait_secs)  # silently drain to avoid PTY backpressure

# ---------- hashing ----------
def sha256(path: str) -> str | None:
    if not os.path.exists(path): return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

# ---------- netem (tc) ----------
def tc_apply_netem(delay: str, loss_pct: float) -> bool:
    """Apply: sudo tc qdisc add dev lo root netem delay <delay> loss <loss>% (or replace if exists)."""
    tc = shutil.which("tc")
    if not tc:
        print("[WARN] 'tc' not found; running without netem.")
        return False
    # Compose args; allow "0ms" and 0% as valid values
    delay_arg = str(delay)
    loss_arg = f"{float(loss_pct)}%"
    cmd = ["sudo", "-n", tc, "qdisc", "add", "dev", "lo", "root", "netem", "delay", delay_arg, "loss", loss_arg]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        # Try replace if already configured
        cmd2 = ["sudo", "-n", tc, "qdisc", "replace", "dev", "lo", "root", "netem", "delay", delay_arg, "loss", loss_arg]
        r2 = subprocess.run(cmd2, capture_output=True, text=True)
        ok = (r2.returncode == 0)
    else:
        ok = True
    if ok:
        print(f"[NETEM] applied: delay {delay_arg}, loss {loss_arg} on lo")
    else:
        print(f"[WARN] failed to apply netem (sudo/tc). Proceeding without netem.")
    return ok

def tc_clear() -> None:
    tc = shutil.which("tc")
    if not tc:
        return
    subprocess.run(["sudo", "-n", tc, "qdisc", "del", "dev", "lo", "root"],
                   capture_output=True, text=True)
    print("[NETEM] cleared qdisc on lo")

# ---------- large foo2 generator ----------
def make_large_foo2(target_bytes: int) -> bool:
    """Create server/foo2 by concatenating server/foo1 until ~target_bytes."""
    src = "server/foo1"
    dst = "server/foo2"
    if not os.path.exists(src):
        print("[X] server/foo1 not found; cannot build large foo2.")
        return False
    try:
        with open(src, "rb") as f:
            chunk = f.read()
        if not chunk:
            chunk = b"x" * 1024  # fallback if foo1 is empty
        with open(dst, "wb") as out:
            written = 0
            while written < target_bytes:
                out.write(chunk)
                written += len(chunk)
            out.flush()
        size = os.path.getsize(dst)
        print(f"[*] Built server/foo2 size ~{size/1024/1024:.1f} MB")
        return True
    except Exception as e:
        print(f"[X] Failed to build large foo2: {e}")
        return False

# ---------- tests (15 pts each, reliability 25 pts) ----------
def test_ls(master_fd: int, timeout: float) -> int:
    out = send_and_capture(master_fd, "ls", read_timeout=timeout)
    print("\n=== Client output (ls) ===")
    print(out.rstrip())
    low = out.lower()
    if all(f in low for f in FOO):
        print("\n[✓] ls contains: foo1, foo2, foo3")
        return 15
    if not out.strip():
        print("\n[X] No client output for ls.")
    else:
        missing = [f for f in FOO if f not in low]
        print("\n[X] Missing in ls output:", ", ".join(missing))
    return 0

def test_get_foo1_by_hash(master_fd: int, wait_secs: float) -> int:
    try: os.remove("client/foo1")
    except FileNotFoundError: pass
    send_and_wait_silent(master_fd, "get foo1", wait_secs)
    h_server = sha256("server/foo1")
    h_client = sha256("client/foo1")
    print(f"server/foo1 SHA256: {h_server or 'N/A'}")
    print(f"client/foo1 SHA256: {h_client or 'N/A'}")
    if h_server is not None and h_client is not None and h_server == h_client:
        print("[✓] Hashes match.")
        return 15
    print("[X] Hash mismatch (or file missing).")
    return 0

def test_delete_foo1_and_ls(master_fd: int, timeout: float, settle: float = 1.0) -> int:
    send_and_wait_silent(master_fd, "delete foo1", settle)
    out = send_and_capture(master_fd, "ls", read_timeout=timeout)
    print("\n=== Client output (ls after delete foo1) ===")
    print(out.rstrip())
    if "foo1" not in out.lower():
        print("\n[✓] 'foo1' not present after delete")
        return 15
    print("\n[X] 'foo1' still present after delete")
    return 0

def test_put_foo1_by_hash(master_fd: int, wait_secs: float) -> int:
    if not os.path.exists("client/foo1"):
        print("[X] client/foo1 not found; cannot put.")
        return 0
    send_and_wait_silent(master_fd, "put foo1", wait_secs)
    h_server = sha256("server/foo1")
    h_client = sha256("client/foo1")
    print(f"server/foo1 SHA256: {h_server or 'N/A'}")
    print(f"client/foo1 SHA256: {h_client or 'N/A'}")
    if h_server is not None and h_client is not None and h_server == h_client:
        print("[✓] Hashes match after put.")
        return 15
    print("[X] Hash mismatch (or file missing) after put.")
    return 0

def test_exit_server(master_fd: int, server_proc: subprocess.Popen, exit_wait: float = 2.0) -> int:
    send_and_wait_silent(master_fd, "exit", exit_wait)
    end = time.time() + exit_wait
    while time.time() < end:
        if server_proc.poll() is not None:
            print(f"[✓] Server exited (return code {server_proc.returncode}).")
            return 15
        time.sleep(0.1)
    print("[X] Server did not exit after 'exit' command.")
    return 0

def test_reliable_under_loss(host: str, port: int, wait_secs: float, size_mb: float, delay: str, loss: float) -> int:
    """Restart server+client, build foo2 of requested size, apply netem delay/loss, get foo2, compare hashes, clear qdisc, stop both."""
    print("\n=== Reliability Test (netem) ===")
    # Build large foo2 BEFORE starting server
    target_bytes = int(size_mb * 1024 * 1024)
    ok_built = make_large_foo2(target_bytes)
    if not ok_built:
        print("[WARN] Could not rebuild foo2; proceeding with existing file (if any).")

    server2 = None
    master2 = None
    client2 = None
    score = 0
    try:
        server2 = start_server(port)
        master2, client2 = start_client_pty(host, port)

        tc_apply_netem(delay, loss)
        try:
            # Ensure a clean client/foo2 before fetch
            try: os.remove("client/foo2")
            except FileNotFoundError: pass

            send_and_wait_silent(master2, "get foo2", wait_secs)

            h_server = sha256("server/foo2")
            h_client = sha256("client/foo2")
            print(f"server/foo2 SHA256: {h_server or 'N/A'}")
            print(f"client/foo2 SHA256: {h_client or 'N/A'}")
            if h_server is not None and h_client is not None and h_server == h_client:
                print(f"[✓] Hashes match under netem (delay {delay}, loss {loss}%).")
                score = 25
            else:
                print("[X] Hash mismatch (or file missing) under netem.")
                score = 0
        finally:
            tc_clear()
    finally:
        if master2 is not None and client2 is not None:
            stop_client_pty(master2, client2)
        stop_proc(server2)
    return score

# ---------- main ----------
def main():
    ap = argparse.ArgumentParser(
        description="UDP grader (single client): ls (15) + get foo1 (15) + delete foo1 (15) + put foo1 (15) + reliability get foo2 under netem (25) + exit (15)."
    )
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--timeout", type=float, default=3.0, help="seconds to read client output for ls")
    ap.add_argument("--wait", type=float, default=5.0, help="seconds to wait after get/put/loss tests")
    ap.add_argument("--delete-wait", type=float, default=1.0, help="seconds to wait after delete")
    ap.add_argument("--exit-wait", type=float, default=2.0, help="seconds to wait for server to exit after 'exit'")
    # New knobs:
    ap.add_argument("--foo2-size-mb", type=float, default=5.0, help="size of server/foo2 for reliability test (MB)")
    ap.add_argument("--loss", type=float, default=5.0, help="packet loss percentage for netem (e.g., 5.0)")
    ap.add_argument("--delay", default="1ms", help="network latency for netem (e.g., '1ms', '5ms')")
    args = ap.parse_args()

    if not make_project():
        print("Score: 0/100"); sys.exit(1)

    total = 0
    server = None
    master = None
    client = None
    try:
        # Start first session
        server = start_server(args.port)
        master, client = start_client_pty(args.host, args.port)

        total += test_ls(master, args.timeout)                          # 15
        total += test_get_foo1_by_hash(master, args.wait)               # 15
        total += test_delete_foo1_and_ls(master, args.timeout, args.delete_wait)  # 15
        total += test_put_foo1_by_hash(master, args.wait)               # 15

        # Exit test (server should exit). Then terminate client per instructions.
        total += test_exit_server(master, server, args.exit_wait)       # 15
        stop_client_pty(master, client)  # terminate client after exit test
        master = client = None

        # Reliability test with fresh server/client under netem (size/loss/delay from args)
        total += test_reliable_under_loss(args.host, args.port, args.wait,
                                          args.foo2_size_mb, args.delay, args.loss)  # 25

        print(f"\n=== Total Score: {total}/100 ===")
    finally:
        if master is not None and client is not None:
            stop_client_pty(master, client)
        stop_proc(server)

if __name__ == "__main__":
    main()

