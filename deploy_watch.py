"""
Watch the mangos build output and redeploy to a server instance.

- Polls BUILD_DIR for new mangosd/realmd .exe/.pdb (size + mtime vs. deployed copy).
- Waits until the build output stops changing (linker finished).
- Gracefully stops the affected server(s) (Ctrl+C on their console, hard kill after a timeout).
  realmd is only stopped when realmd itself changed.
- Copies .exe + .pdb, and replaces SERVER_DIR/statetree with a fresh copy of the PlayerBots statetree.
- Starts realmd first (if it was stopped / not running), then mangosd, each in its own console.

Usage:
    python deploy_watch.py                 # watch forever
    python deploy_watch.py --once          # deploy if needed, then exit
    python deploy_watch.py --dry-run       # only report what would happen
    python deploy_watch.py --watch-statetree   # also redeploy when only statetree files changed
"""

import argparse
import ctypes
import ctypes.wintypes
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime

BUILD_DIR = r"F:\progs\mangos\build\bin\x64_RelWithDebInfo"
SERVER_DIR = r"F:\server\vanillanew"
STATETREE_SRC = r"F:\progs\mangos\src\modules\PlayerBots\statetree"
STATETREE_DST = os.path.join(SERVER_DIR, "statetree")

# Start order; stop order is the reverse.
SERVERS = ["realmd", "mangosd"]

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
SYNCHRONIZE = 0x00100000
PROCESS_TERMINATE = 0x0001
WAIT_OBJECT_0 = 0
CREATE_NEW_CONSOLE = 0x00000010
DETACHED_PROCESS = 0x00000008

kernel32.OpenProcess.restype = ctypes.wintypes.HANDLE
kernel32.OpenProcess.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.DWORD]
kernel32.WaitForSingleObject.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD]
kernel32.TerminateProcess.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.UINT]
kernel32.CloseHandle.argtypes = [ctypes.wintypes.HANDLE]

# Runs in a separate, console-less python process: attaches to the target's console and sends Ctrl+C.
# mangosd and realmd both handle SIGINT as a graceful shutdown (players are saved).
CTRL_C_HELPER = r"""
import ctypes, sys
k = ctypes.WinDLL("kernel32")
k.FreeConsole()
if not k.AttachConsole(int(sys.argv[1])):
    sys.exit(1)
k.SetConsoleCtrlHandler(None, True)
sys.exit(0 if k.GenerateConsoleCtrlEvent(0, 0) else 2)
"""


def log(msg):
    print(f"[{datetime.now():%Y-%m-%d %H:%M:%S}] {msg}", flush=True)


# ---------------------------------------------------------------- file state

def file_sig(path):
    try:
        st = os.stat(path)
        return st.st_size, int(st.st_mtime)
    except FileNotFoundError:
        return None


def binaries(name):
    return [f"{name}.exe", f"{name}.pdb"]


def build_sigs():
    return {f: file_sig(os.path.join(BUILD_DIR, f)) for name in SERVERS for f in binaries(name)}


def tree_sig(root):
    sig = {}
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            sig[os.path.relpath(p, root).lower()] = file_sig(p)
    return sig


def differs(src_sig, dst_sig):
    if src_sig is None:
        return False  # nothing built
    if dst_sig is None:
        return True
    # copy2 preserves mtime; allow FS timestamp granularity slack
    return src_sig[0] != dst_sig[0] or abs(src_sig[1] - dst_sig[1]) > 2


def changed_servers():
    changed = []
    for name in SERVERS:
        for f in binaries(name):
            if differs(file_sig(os.path.join(BUILD_DIR, f)), file_sig(os.path.join(SERVER_DIR, f))):
                changed.append(name)
                break
    return changed


def statetree_changed():
    src, dst = tree_sig(STATETREE_SRC), tree_sig(STATETREE_DST)
    if src.keys() != dst.keys():
        return True
    return any(differs(src[k], dst[k]) for k in src)


def wait_for_stable_build(settle):
    """Wait until build outputs have not changed for `settle` seconds."""
    last = build_sigs()
    stable_since = time.time()
    while time.time() - stable_since < settle:
        time.sleep(1)
        cur = build_sigs()
        if cur != last:
            last, stable_since = cur, time.time()


# ---------------------------------------------------------------- processes

def find_processes(name):
    """PIDs of <name>.exe running from SERVER_DIR."""
    exe = f"{name}.exe"
    cmd = (f"Get-CimInstance Win32_Process -Filter \"Name='{exe}'\" | "
           "Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        return []
    procs = json.loads(out)
    if isinstance(procs, dict):
        procs = [procs]
    target = os.path.normcase(os.path.join(SERVER_DIR, exe))
    pids = []
    for p in procs:
        path = p.get("ExecutablePath")
        if path is None:
            # path hidden (e.g. elevated process and we are not elevated) - assume it is ours
            log(f"  warning: can't read path of {exe} pid {p['ProcessId']}, assuming it is ours")
            pids.append(p["ProcessId"])
        elif os.path.normcase(path) == target:
            pids.append(p["ProcessId"])
    return pids


def wait_pid(pid, timeout_s):
    """True if the process is gone within timeout."""
    h = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if not h:
        return True  # already gone
    try:
        return kernel32.WaitForSingleObject(h, int(timeout_s * 1000)) == WAIT_OBJECT_0
    finally:
        kernel32.CloseHandle(h)


def kill_pid(pid):
    h = kernel32.OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, False, pid)
    if not h:
        return
    try:
        kernel32.TerminateProcess(h, 1)
        kernel32.WaitForSingleObject(h, 10000)
    finally:
        kernel32.CloseHandle(h)


def stop_server(name, timeout_s):
    """Stops all instances of name; returns True if anything was running."""
    pids = find_processes(name)
    for pid in pids:
        log(f"  stopping {name}.exe (pid {pid}) with Ctrl+C...")
        rc = subprocess.run([sys.executable, "-c", CTRL_C_HELPER, str(pid)],
                            creationflags=DETACHED_PROCESS).returncode
        if rc != 0:
            log(f"  could not send Ctrl+C (rc={rc})")
        if rc != 0 or not wait_pid(pid, timeout_s):
            log(f"  {name}.exe did not exit in time, killing it")
            kill_pid(pid)
        log(f"  {name}.exe stopped")
    return bool(pids)


def start_server(name):
    exe = os.path.join(SERVER_DIR, f"{name}.exe")
    log(f"  starting {name}.exe")
    p = subprocess.Popen([exe], cwd=SERVER_DIR, creationflags=CREATE_NEW_CONSOLE)
    time.sleep(3)
    if p.poll() is not None:
        log(f"  WARNING: {name}.exe exited immediately with code {p.returncode}")


# ---------------------------------------------------------------- copying

def copy_file(src, dst, retries=10):
    tmp = dst + ".deploy_tmp"
    for attempt in range(retries):
        try:
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
            return
        except PermissionError as e:
            log(f"  {os.path.basename(dst)} locked ({e.strerror}), retrying...")
            time.sleep(2)
    raise RuntimeError(f"could not copy {src} -> {dst}")


def deploy_statetree():
    log(f"  replacing {STATETREE_DST}")
    if os.path.isdir(STATETREE_DST):
        shutil.rmtree(STATETREE_DST)
    shutil.copytree(STATETREE_SRC, STATETREE_DST)


# ---------------------------------------------------------------- main

def deploy(changed, args):
    log(f"deploying: {', '.join(changed)}")
    if args.dry_run:
        log("  (dry run - nothing done)")
        return

    # mangosd always goes down when statetree changes, since it only reads the trees on startup
    to_stop = [n for n in reversed(SERVERS) if n in changed]

    stopped = set()
    for name in to_stop:
        if stop_server(name, args.shutdown_timeout):
            stopped.add(name)

    for name in SERVERS:
        if name not in changed:
            continue
        for f in binaries(name):
            src = os.path.join(BUILD_DIR, f)
            if os.path.exists(src):
                log(f"  copying {f}")
                copy_file(src, os.path.join(SERVER_DIR, f))
        if name == "mangosd":
            deploy_statetree()

    for name in SERVERS:
        running = bool(find_processes(name))
        if not running and (name in stopped or name in changed or name == "realmd"):
            start_server(name)
    log("deploy done")


def check(args):
    changed = changed_servers()
    if args.watch_statetree and "mangosd" not in changed and statetree_changed():
        log("statetree changed")
        changed.append("mangosd")
    if not changed:
        return
    log(f"new build detected ({', '.join(changed)}), waiting {args.settle}s for build to settle...")
    wait_for_stable_build(args.settle)
    changed = changed_servers()  # re-evaluate after the build finished
    if args.watch_statetree and "mangosd" not in changed and statetree_changed():
        changed.append("mangosd")
    if changed:
        deploy(changed, args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--interval", type=float, default=5, help="poll interval in seconds (default 5)")
    ap.add_argument("--settle", type=float, default=10,
                    help="seconds the build output must stay unchanged before deploying (default 10)")
    ap.add_argument("--shutdown-timeout", type=float, default=120,
                    help="seconds to wait for a graceful shutdown before killing (default 120)")
    ap.add_argument("--watch-statetree", action="store_true",
                    help="also redeploy mangosd when only statetree files changed")
    ap.add_argument("--once", action="store_true", help="check once and exit")
    ap.add_argument("--dry-run", action="store_true", help="report only, do not stop/copy/start")
    args = ap.parse_args()

    log(f"watching {BUILD_DIR} -> {SERVER_DIR}")
    while True:
        try:
            check(args)
        except Exception as e:
            log(f"ERROR: {e}")
        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
