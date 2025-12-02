#!/usr/bin/env python3
"""
PA4 Grading Script (Dynamic CWD + Port Cleanup)

Tests & Scoring
- Test 1: 4 servers  (50 pts total)
  - 40 pts: hashes match AND pieces = 4/server (16 total)
  - 10 pts: `./dfc list` shows both files (output printed)
- Test 2: 3 servers  (20 pts total)
  - Kill server 4; list shows both (+10)
  - Fresh GETs; hashes still match (+10)
- Test 3: 2 servers  (20 pts total)
  - Kill server 2; list shows both (+10)
  - Fresh GETs; hashes still match (+10)
- Test 4: 1 server   (10 pts total)
  - Kill server 3 (only server 1 alive); `list` shows "(incomplete)" (relaxed) and GET prints "<filename> is incomplete" for both (+10)
- Test 5: Multiple DFCs (extra) (10 pts total)
  - Restart all 4 servers; run 4 concurrent clients in dfc1..dfc4; copy dfc + dfc.conf into each; all 4 GET wine3.jpg; all 4 hashes match sample (+10)

Total: 110 pts

Usage:
    python3 grade_pa4.py <port_1> <port_2> <port_3> <port_4>
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import urlretrieve

# === Runtime paths ===
PA4_DIR = Path.cwd()
DFS_DIRS = [PA4_DIR / f"dfs{i}" for i in range(1, 5)]
SAMPLE_DIR = PA4_DIR / "sample_file"
SAMPLES = {
    "wine3.jpg": "http://netsys.cs.colorado.edu/images/wine3.jpg",
    "apple_ex.png": "http://netsys.cs.colorado.edu/images/apple_ex.png",
}
CLIENT = "./dfc"
SERVER = "./dfs"

# === Utilities ===

def run(cmd, cwd, timeout=20, check=True, quiet=False):
    """Run a command and return (rc, stdout, stderr)."""
    try:
        proc = subprocess.run(
            cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout
        )
        if check and proc.returncode != 0:
            raise subprocess.CalledProcessError(proc.returncode, cmd, proc.stdout, proc.stderr)
        if quiet:
            return proc.returncode, "", ""
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        return 124, ("" if quiet else (e.stdout or "")), ("" if quiet else (e.stderr or f"Timeout after {timeout}s"))

def ensure_dir_empty(path: Path):
    """Ensure directory exists and is empty."""
    if path.exists():
        for child in list(path.iterdir()):
            try:
                if child.is_file() or child.is_symlink():
                    child.unlink()
                elif child.is_dir():
                    shutil.rmtree(child, ignore_errors=True)
            except Exception:
                pass
    else:
        path.mkdir(parents=True, exist_ok=True)

def hash_file(path: Path, algo="md5") -> str:
    """Compute file hash."""
    h = hashlib.new(algo)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()

def kill_port(port: int):
    """Kill any process currently listening on the specified port."""
    # Try lsof
    try:
        proc = subprocess.run(["lsof", "-ti", f"tcp:{port}"], capture_output=True, text=True)
        pids = [p.strip() for p in proc.stdout.splitlines() if p.strip()]
        for pid in pids:
            print(f"    - Killing process {pid} on port {port}")
            subprocess.run(["kill", "-9", pid], capture_output=True)
        return
    except FileNotFoundError:
        pass
    # Fallback: fuser
    subprocess.run(["fuser", "-k", f"{port}/tcp"], capture_output=True)

def kill_bound_ports(ports):
    print("[*] Cleaning up any existing processes bound to target ports ...")
    for p in ports:
        kill_port(p)
    time.sleep(0.5)

def write_dfc_conf(ports):
    conf_lines = [f"server dfs{i+1} 127.0.0.1:{p}" for i, p in enumerate(ports)]
    (PA4_DIR / "dfc.conf").write_text("\n".join(conf_lines) + "\n")

def start_servers(ports):
    procs = []
    for i, port in enumerate(ports, start=1):
        dirpath = PA4_DIR / f"dfs{i}"
        cmd = [SERVER, str(dirpath), str(port)]
        p = subprocess.Popen(cmd, cwd=PA4_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        procs.append(p)
        print(f"    - Started dfs{i} on port {port}")
    time.sleep(1.5)
    return procs

def stop_servers(procs):
    for p in procs:
        try:
            p.terminate()
        except Exception:
            pass
    time.sleep(0.5)
    for p in procs:
        if p.poll() is None:
            try:
                p.kill()
            except Exception:
                pass

def ensure_samples():
    print("[*] Ensuring sample files ...")
    SAMPLE_DIR.mkdir(exist_ok=True)
    for fname, url in SAMPLES.items():
        dest = SAMPLE_DIR / fname
        if not dest.exists():
            print(f"    - Downloading {fname}")
            try:
                urlretrieve(url, dest)
            except Exception as e:
                print(f"[err] Failed to download {fname}: {e}")

def put_samples():
    print("[*] Uploading (PUT) ...")
    for fname in SAMPLES:
        src = SAMPLE_DIR / fname
        rc, out, err = run([CLIENT, "put", str(src)], cwd=PA4_DIR, timeout=60, check=False)
        if out.strip():
            print(out.strip())
        if err.strip():
            print(err.strip())
        if rc != 0:
            print(f"[warn] PUT failed for {fname}: {err.strip()}")

def get_files(fnames):
    """GET files into PA4_DIR; return True if all rc==0."""
    print("[*] Downloading (GET) ...")
    all_ok = True
    for fname in fnames:
        rc, out, err = run([CLIENT, "get", fname], cwd=PA4_DIR, timeout=60, check=False)
        if out.strip():
            print(out.strip())
        if err.strip():
            print(err.strip())
        if rc != 0:
            all_ok = False
            print(f"[warn] GET failed for {fname}: {err.strip()}")
    return all_ok

def verify_hashes(fnames, label=""):
    """Compare hashes of PA4_DIR/fname vs SAMPLE_DIR/fname; return True if all match."""
    print(f"[*] Checking hashes {label}...")
    ok = True
    for fname in fnames:
        ref = SAMPLE_DIR / fname
        got = PA4_DIR / fname
        if not (ref.exists() and got.exists()):
            ok = False
            print(f"[warn] Missing file for hash compare {label}: {fname}")
            continue
        ref_md5 = hash_file(ref)
        got_md5 = hash_file(got)
        print(f"    - {fname}: {ref_md5} vs {got_md5}")
        if ref_md5 != got_md5:
            ok = False
            print(f"[warn] Hash mismatch {label} for {fname}")
    return ok

def count_pieces():
    """Return dict of piece counts per server and total."""
    piece_counts = {}
    total_pieces = 0
    for d in DFS_DIRS:
        cnt = sum(1 for _ in d.rglob("*") if _.is_file())
        piece_counts[d.name] = cnt
        total_pieces += cnt
        print(f"    - {d.name}: {cnt} files")
    return piece_counts, total_pieces

def print_list_output(tag):
    print(f"[*] Running './dfc list' {tag} ...")
    rc, out, err = run([CLIENT, "list"], cwd=PA4_DIR, timeout=30, check=False)
    print(f"---- ./dfc list output {tag} ----")
    if out.strip():
        print(out.strip())
    if err.strip():
        print("[stderr]", err.strip())
    print("---------------------------------")
    list_ok = (rc == 0 and ("apple_ex.png" in (out or "").lower()) and ("wine3.jpg" in (out or "").lower()))
    return list_ok, rc, out, err

def print_list_output_expect_incomplete(tag):
    """Run list and expect '<filename> [incomplete]' (relaxed). Returns bool."""
    print(f"[*] Running './dfc list' {tag} (expect <filename> [incomplete]) ...")
    rc, out, err = run([CLIENT, "list"], cwd=PA4_DIR, timeout=30, check=False)
    print(f"---- ./dfc list output {tag} ----")
    if out.strip():
        print(out.strip())
    if err.strip():
        print("[stderr]", err.strip())
    print("---------------------------------")

    # Relaxed matching: check per-line that the filename and 'incomplete' appear together, case-insensitive.
    out_text = (out or "")
    lines = [ln.strip().lower() for ln in out_text.splitlines() if ln.strip()]
    def line_has_incomplete_for(fname):
        fl = fname.lower()
        for ln in lines:
            if fl in ln and "incomplete" in ln:
                return True
        return False

    ok = all(line_has_incomplete_for(fn) for fn in ("apple_ex.png", "wine3.jpg"))
    return ok, rc, out, err

def ensure_server_running(procs, idx, port):
    """Ensure dfs{idx+1} is running; if not, start it and store handle in procs[idx]."""
    try:
        running = (procs[idx] is not None) and (procs[idx].poll() is None)
    except Exception:
        running = False
    if not running:
        dirpath = PA4_DIR / f"dfs{idx+1}"
        cmd = [SERVER, str(dirpath), str(port)]
        p = subprocess.Popen(cmd, cwd=PA4_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        procs[idx] = p
        print(f"    - (re)started dfs{idx+1} on port {port}")
    return procs

# === Main ===

def main():
    print("=== Prepare Test Environment ===")
    parser = argparse.ArgumentParser()
    parser.add_argument("port_1", type=int)
    parser.add_argument("port_2", type=int)
    parser.add_argument("port_3", type=int)
    parser.add_argument("port_4", type=int)
    args = parser.parse_args()

    ports = [args.port_1, args.port_2, args.port_3, args.port_4]
    print(f"[*] Current working directory: {PA4_DIR}")

    # Kill any process using the ports
    kill_bound_ports(ports)

    # Build project (suppress output/warnings)
    print("[*] Building project (make clean && make) ...")
    run(["make", "-s", "clean"], cwd=PA4_DIR, timeout=60, check=False, quiet=True)
    rc, _, _ = run(["make", "-s"], cwd=PA4_DIR, timeout=180, check=True, quiet=True)
    if rc == 0:
        print("    - Build OK")

    # Prepare directories
    print("[*] Clearing dfs directories ...")
    for d in DFS_DIRS:
        ensure_dir_empty(d)
        print(f"    - {d} ready")

    # Write config
    write_dfc_conf(ports)
    print("[*] Wrote dfc.conf:")
    print((PA4_DIR / "dfc.conf").read_text())

    # Start servers
    print("[*] Starting servers ...")
    procs = start_servers(ports)
    time.sleep(1)

    # Initialize scoring
    test1_score = 0  # /50
    test2_score = 0  # /20
    test3_score = 0  # /20
    test4_score = 0  # /10
    test5_score = 0  # /10

    try:
        # Ensure samples and PUT
        ensure_samples()
        print("\n")
        print("=== Test 1: 4 servers ===")
        put_samples()

        # --- Test 1 actions ---
        got_ok = get_files(SAMPLES.keys())
        hash_ok = verify_hashes(SAMPLES.keys(), "(Test 1)")

        print("[*] Checking server piece counts (Test 1) ...")
        piece_counts, total_pieces = count_pieces()
        pieces_ok = (total_pieces == 16 and all(c == 4 for c in piece_counts.values()))
        if not pieces_ok:
            print(f"[warn] Expected 16 total pieces (4 per server), got {piece_counts} (total {total_pieces}).")

        if hash_ok and pieces_ok:
            test1_score += 40

        list_ok, _, _, _ = print_list_output("(Test 1)")
        if list_ok:
            test1_score += 10
            print("    - List shows both files (+10 pts)")
        else:
            print("[warn] `dfc list` (Test 1) did not include both files.")

        # --- Test 2: kill server 4, list + fresh GET + hash ---
        print("\n=== Test 2: 3 servers (terminate server 4) ===")
        try:
            p4 = procs[3]
            if p4 and (p4.poll() is None):
                print("[*] Terminating dfs4 ...")
                p4.terminate()
                time.sleep(0.5)
                if p4.poll() is None:
                    p4.kill()
                print("    - dfs4 terminated")
            else:
                print("    - dfs4 already not running")
        except Exception as e:
            print(f"[warn] Could not terminate dfs4 cleanly: {e}")

        list2_ok, _, _, _ = print_list_output("(Test 2)")
        if list2_ok:
            test2_score += 10
        else:
            print("[warn] `dfc list` (Test 2) did not include both files.")

        # Remove local copies to force fresh GETs
        for fname in SAMPLES:
            fpath = PA4_DIR / fname
            if fpath.exists():
                try:
                    fpath.unlink()
                except Exception:
                    pass

        got2_ok = get_files(SAMPLES.keys())
        hash2_ok = verify_hashes(SAMPLES.keys(), "(Test 2)")
        if got2_ok and hash2_ok:
            test2_score += 10

        # --- Test 3: kill server 2, list + fresh GET + hash ---
        print("\n=== Test 3: 2 servers (terminate server 2) ===")
        try:
            p2 = procs[1]
            if p2 and (p2.poll() is None):
                print("[*] Terminating dfs2 ...")
                p2.terminate()
                time.sleep(0.5)
                if p2.poll() is None:
                    p2.kill()
                print("    - dfs2 terminated")
            else:
                print("    - dfs2 already not running")
        except Exception as e:
            print(f"[warn] Could not terminate dfs2 cleanly: {e}")

        list3_ok, _, _, _ = print_list_output("(Test 3)")
        if list3_ok:
            test3_score += 10
        else:
            print("[warn] `dfc list` (Test 3) did not include both files.")

        # Remove local copies to force fresh GETs
        for fname in SAMPLES:
            fpath = PA4_DIR / fname
            if fpath.exists():
                try:
                    fpath.unlink()
                except Exception:
                    pass

        got3_ok = get_files(SAMPLES.keys())
        hash3_ok = verify_hashes(SAMPLES.keys(), "(Test 3)")
        if got3_ok and hash3_ok:
            test3_score += 10

        # --- Test 4: 1 server left (kill server 3), expect 'incomplete' in list & get ---
        print("\n=== Test 4: 1 server (terminate server 3) ===")
        try:
            p3 = procs[2]
            if p3 and (p3.poll() is None):
                print("[*] Terminating dfs3 ...")
                p3.terminate()
                time.sleep(0.5)
                if p3.poll() is None:
                    p3.kill()
                print("    - dfs3 terminated")
            else:
                print("    - dfs3 already not running")
        except Exception as e:
            print(f"[warn] Could not terminate dfs3 cleanly: {e}")

        # Now only dfs1 should be alive. Check list shows '[incomplete]' (relaxed)
        list4_ok, _, _, _ = print_list_output_expect_incomplete("(Test 4)")

        # Remove local copies to force GET output messages
        for fname in SAMPLES:
            fpath = PA4_DIR / fname
            if fpath.exists():
                try:
                    fpath.unlink()
                except Exception:
                    pass

        # GET should state '<filename> is incomplete' for both (relaxed check)
        print("[*] Attempting GET under 1-server scenario (expect '<filename> is incomplete') ...")
        get_incomplete_ok = True
        for fname in SAMPLES:
            rcg4, outg4, errg4 = run([CLIENT, "get", fname], cwd=PA4_DIR, timeout=60, check=False)
            text = ((outg4 or "") + "\n" + (errg4 or "")).lower()
            # Print output for student's reference
            if outg4.strip():
                print(outg4.strip())
            if errg4.strip():
                print(errg4.strip())
            if (fname.lower() not in text) or ("incomplete" not in text):
                get_incomplete_ok = False

        if list4_ok and get_incomplete_ok:
            test4_score += 10

        # --- Test 5: Multiple DFCs (extra credit: 10 pts) ---
        print("\n=== Test 5: Multiple DFCs (4 concurrent clients) ===")
        # Ensure all 4 servers are up again for a full reconstruction
        for idx, port in enumerate(ports):
            procs = ensure_server_running(procs, idx, port)
        time.sleep(1.0)

        # Prepare dfc1..dfc4 directories and copy ./dfc and dfc.conf into each
        dfc_dirs = [PA4_DIR / f"dfc{i}" for i in range(1, 5)]
        for d in dfc_dirs:
            d.mkdir(parents=True, exist_ok=True)
            # Clean prior wine3.jpg
            got_file = d / "wine3.jpg"
            if got_file.exists():
                try:
                    got_file.unlink()
                except Exception:
                    pass
            # Copy client executable
            try:
                src = PA4_DIR / "dfc"
                dst = d / "dfc"
                if dst.exists():
                    dst.unlink()
                dst.write_bytes(src.read_bytes())
                dst.chmod(0o755)
            except Exception as e:
                print(f"[warn] Could not prepare dfc in {d}: {e}")
            # Copy dfc.conf
            try:
                conf_src = PA4_DIR / "dfc.conf"
                conf_dst = d / "dfc.conf"
                conf_dst.write_bytes(conf_src.read_bytes())
            except Exception as e:
                print(f"[warn] Could not copy dfc.conf to {d}: {e}")

        # Run concurrent GETs
        procs_get = []
        for d in dfc_dirs:
            try:
                pget = subprocess.Popen(
                    ["./dfc", "get", "wine3.jpg"],
                    cwd=d,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                procs_get.append((d, pget))
            except Exception as e:
                print(f"[warn] Could not start dfc in {d}: {e}")

        # Wait for all to finish
        for d, pget in procs_get:
            try:
                outg, errg = pget.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                pget.kill()
                outg, errg = pget.communicate()
            print(f"---- Output from {d.name} ----")
            if outg.strip():
                print(outg.strip())
            if errg.strip():
                print("[stderr]", errg.strip())
            print("------------------------------")

        # Verify hashes
        ref = SAMPLE_DIR / "wine3.jpg"
        if not ref.exists():
            print("[warn] Missing sample wine3.jpg for Test 5 hash compare.")
        else:
            ref_md5 = hash_file(ref)
            hashes_match = True
            for d in dfc_dirs:
                got = d / "wine3.jpg"
                if not got.exists():
                    print(f"[warn] Missing {got} in Test 5.")
                    hashes_match = False
                    continue
                got_md5 = hash_file(got)
                print(f"    - {d.name}/wine3.jpg: {got_md5}")
                if got_md5 != ref_md5:
                    hashes_match = False
            if hashes_match:
                test5_score = 10
                print("    - All four hashes match sample (+10 pts)")

        # Print per-test results
        print("\n==== RESULT (Test 1) ====")
        print(f"Score: {test1_score} / 50")
        print("\n==== RESULT (Test 2) ====")
        print(f"Score: {test2_score} / 20")
        print("\n==== RESULT (Test 3) ====")
        print(f"Score: {test3_score} / 20")
        print("\n==== RESULT (Test 4) ====")
        print(f"Score: {test4_score} / 10")
        print("\n==== RESULT (Test 5) ====")
        print(f"Score: {test5_score} / 10")

        total = test1_score + test2_score + test3_score + test4_score + test5_score
        print("\n==== TOTAL ====")
        print(f"Total Score: {total} / 110")

    finally:
        print("[*] Stopping servers ...")
        stop_servers(procs)

if __name__ == "__main__":
    main()
