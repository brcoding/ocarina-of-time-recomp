#!/usr/bin/env python3
# Written for ocarina-of-time-recomp (MIT, Copyright (c) 2026 Jeff Aigner).
# The instance handling (profile copy, OOT_INPUT=none, OOT_BACKGROUND_WINDOW, the measured-run lock) follows
# metroid-prime-4-recomp tools/run_game.py @ 586e91e1b6f38c9e512e39f2d47a046fdf273cf0 (MIT, Copyright (c) 2026 Jeff Aigner).
"""Run oot.exe for a test: the environment passed through, the log numbered, the game killed afterwards, and several
runs at once (from several sessions or worktrees) kept out of each other's way.

    python3 tools/run.py [--seconds 60] [--shot PNG] [--press-a] [--env OOT_X=1 ...] [--fresh-cache NAME]
                         [--dump FRAME] [--trace gfx] [--volume 60] [--retry 3] [--expect REGEX ...]
                         [--instance NAME] [--profile DIR] [--live-input] [--alone] [-- extra args]
    python3 tools/run.py --list

What it takes care of, each a mistake made by hand at least once:
  - Every OOT_* variable in the child's environment is named in WSLENV (path-valued ones with /p). oot.exe is a
    Windows program and silently ignores the rest.
  - Each run is an instance that ends its own game and nothing else: oot.exe and the oot_xlate.exe children it started
    are killed after the run (the exit path can hang). From Windows Python the game runs in a job object, which also
    ends it when run.py itself is killed; from WSL the game is found by its command line, and SIGTERM or SIGHUP to
    run.py end it too (SIGKILL cannot). Killing every oot.exe by
    name, as run.py did before, ended other sessions' runs.
  - Instances register in build/logs/instances/<name>/ (a worktree's build/logs is a junction to the main checkout's,
    so every checkout sees the same list): --list shows the live ones. Two live instances never share a name, a
    profile, a --fresh-cache folder, a --record file or a --shot path; the second refuses to start (--kill-existing
    ends the first instead). Log numbers are taken atomically, so concurrent runs never write the same run-<n>.log.
  - Each instance plays on its own copy of build/profile (saves, account), deleted afterwards. The game empties temp/
    and save-tx/ when it starts, so two games on one profile corrupt each other's saves; and test runs leave the
    player's saves alone. --profile DIR keeps the saves of a series of runs in DIR (seeded from build/profile when new).
  - The real gamepad and keyboard are off (OOT_INPUT=none; input scripts still play): every game reads the pad whether
    or not its window has focus, so the player's input would reach test runs. --live-input (implied by --record and
    OOT_TEST_PAD) turns them on.
  - The window opens behind the others without flashing and never goes fullscreen (OOT_BACKGROUND_WINDOW=1), and the
    game keeps counting itself in focus (OOT_ASSUME_FOCUS=1): out of focus it pauses itself, as on the console under
    the HOME menu. --no-focus-pin to test that.
  - VRAM cannot be isolated, only rationed: a game holds about 1.4 GB at the title and 12 GB at the level, of a
    Vulkan budget of about 14 GB on the 16 GB card, and two games at the level would page textures out or fail their
    allocations. A run waits until its --vram (default 12 GB with --replay, which goes to the level, else 2 GB) fits
    next to the running instances' (OOT_VRAM_GB=14; a game run.py did not start counts 12), and at most OOT_MAX_RUNS
    (3) games run. One game at the level also commits about 24 GB. Frame rates and GPU times taken next to another game
    are not comparable: the log and the summary say how many ran alongside, and --alone waits until no other game
    runs and holds new runs back until it is done.
  - A run that exits non-zero within its first 25 s is run again, up to --retry times (once for the start-up crash,
    fixed since: docs/phase3.md); --retry 0 shows every crash. A later crash is reported, not retried.
  - --press-a presses A at frame 600, which brings up the title menu over the cinematic. --record FILE writes what
    the player does (pad and keyboard) as an input script, --replay FILE plays one back; --skip-movies ends every
    movie after the title loop at once (OOT_MOVIE_SKIP=2), so a replay reaches the game without the opening.
  - --fresh-cache NAME translates into an empty build/logs/sc-NAME, for runs whose lowering differs in ways the
    cache keys do not show (OOT_TENSOR_ONLY, OOT_TENSOR_HMMA_MASK).
  - --expect REGEX checks the log for each pattern afterwards (did the switch apply?) and fails the run if one is
    missing; a diagnostic that never applied cost a whole bisection on 2026-11-06.
Instances of one build dir share its shader and pipeline caches; the runtime appends to the record files under a
file lock and merges the pipeline cache other instances saved (gfx_shader.c, vk_pipeline.c). A worktree whose
tools/run.py predates instances still kills every oot.exe after its runs: update it before running alongside.
Logs: build/logs/run-<n>.log; --dump writes build/logs/dump-<n>/. Prints a summary of the log (frame rate, GPU time,
movie pacing, tensor cores, crashes) and exits non-zero when the run failed or an --expect was not met.
"""
import argparse
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

if os.name == "nt":
    import ctypes
    import msvcrt
    from ctypes import wintypes
else:
    import fcntl

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "v131072" / "oot.exe"
LOGS = ROOT / "build" / "logs"
INSTANCES = LOGS / "instances"   # in build/logs, which worktrees share through a junction
PROFILE = ROOT / "build" / "profile"
PATH_VARS = {"OOT_SHADER_CACHE", "OOT_INPUT_SCRIPT", "OOT_INPUT_RECORD", "OOT_DUMP_DIR", "OOT_XLATE", "OOT_PROFILE_DIR", "OOT_SHOTS_DIR",
             "OOT_FLIGHT_DIR", "OOT_RDOC_DIR"}
HOST = "windows" if os.name == "nt" else "wsl"
MAX_RUNS = max(1, int(os.environ.get("OOT_MAX_RUNS", "3")))
# VRAM is shared by every game on the card, so runs are admitted against a budget: the Vulkan budget of the 16 GB
# RX 9070 XT is about 14 GB; a game holds about 1.4 GB at the title and 12 GB at the level (run-46, OOT_TRACE=perf).
VRAM_BUDGET = float(os.environ.get("OOT_VRAM_GB", "14"))
LEVEL_GB, TITLE_GB = 12.0, 2.0
LOCK_OFFSET = 1 << 16   # the Windows byte-range lock sits past the file's text, so other processes can still read it
PROFILE_SKIP = {"temp", "save-tx", "save-backup"}   # emptied by the game at start, or backups it never reads


# ---- locks: an OS lock on an open file, dropped by the OS when the holder exits or dies

def try_lock(f) -> bool:
    try:
        if os.name == "nt":
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def unlock(f) -> None:
    if f is None:
        return
    try:
        if os.name == "nt":
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    except OSError:
        pass
    f.close()


def lock_wait(path: Path, message: str | None = None, poll: float = 0.1):
    """Blocks until this process holds the lock on path; returns the open file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    said = False
    while True:
        f = open(path, "a+b")
        if try_lock(f):
            return f
        f.close()
        if message and not said:
            print(message, flush=True)
            said = True
        time.sleep(poll)


def held(path: Path) -> bool:
    """True while another process (or another open file of this one) holds the lock on path."""
    if not path.exists():
        return False
    try:
        f = open(path, "a+b")
    except OSError:   # its folder is being deleted
        return False
    if try_lock(f):
        unlock(f)
        return False
    f.close()
    return True


# ---- processes

def tasks() -> dict[int, str]:
    """Windows PID -> lower-case image name of every process (tasklist works from Windows and from WSL)."""
    out = subprocess.run(["tasklist.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True, errors="replace").stdout
    procs = {}
    for row in csv.reader(out.splitlines()):
        if len(row) >= 2 and row[1].isdigit():
            procs[int(row[1])] = row[0].lower()
    return procs


def games() -> set[int]:
    return {pid for pid, name in tasks().items() if name == "oot.exe"}


def powershell(cmd: str) -> str:
    return subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", cmd], capture_output=True,
                          text=True, errors="replace").stdout


def game_pid_by_marker(marker: str) -> int | None:
    """The Windows PID of the oot.exe whose command line holds marker (this instance's --profile)."""
    out = powershell("Get-CimInstance Win32_Process -Filter \"Name='oot.exe'\" | "
                     "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }")
    for line in out.splitlines():
        pid, _, cmd = line.partition("\t")
        if pid.strip().isdigit() and marker in cmd.replace("\\", "/"):
            return int(pid)
    return None


def kill_tree(pid: int) -> None:
    """Ends a game by Windows PID with the translator children it started, also those it left behind when it died."""
    subprocess.run(["taskkill.exe", "/F", "/T", "/PID", str(pid)], capture_output=True)
    powershell(f"Get-CimInstance Win32_Process -Filter \"Name='oot_xlate.exe' AND ParentProcessId={pid}\" | "
               "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }")


if os.name == "nt":
    class _BasicLimit(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class _ExtendedLimit(ctypes.Structure):
        _fields_ = [("Basic", _BasicLimit), ("IoInfo", ctypes.c_uint64 * 6), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    class _Accounting(ctypes.Structure):
        _fields_ = [("TotalUserTime", ctypes.c_int64), ("TotalKernelTime", ctypes.c_int64),
                    ("ThisPeriodTotalUserTime", ctypes.c_int64), ("ThisPeriodTotalKernelTime", ctypes.c_int64),
                    ("TotalPageFaultCount", wintypes.DWORD), ("TotalProcesses", wintypes.DWORD),
                    ("ActiveProcesses", wintypes.DWORD), ("TotalTerminatedProcesses", wintypes.DWORD)]

    class _MemStatus(ctypes.Structure):
        _fields_ = [("dwLength", wintypes.DWORD), ("dwMemoryLoad", wintypes.DWORD)] + \
                   [(n, ctypes.c_uint64) for n in ("TotalPhys", "AvailPhys", "TotalPageFile", "AvailPageFile",
                                                   "TotalVirtual", "AvailVirtual", "AvailExtendedVirtual")]

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
    _k32.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD)
    _k32.QueryInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD,
                                               wintypes.LPVOID)
    _k32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    _k32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    _k32.CloseHandle.argtypes = (wintypes.HANDLE,)
    _k32.GlobalMemoryStatusEx.argtypes = (ctypes.POINTER(_MemStatus),)


class Job:
    """A Windows job object holding the game and every oot_xlate.exe it starts: kill() ends them all, orphans included,
    and Windows ends them when the last handle closes, so a run.py that is killed takes its game along."""

    def __init__(self, pid: int):
        self.h = _k32.CreateJobObjectW(None, None)
        info = _ExtendedLimit()
        info.Basic.LimitFlags = 0x2000   # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        ok = bool(self.h) and bool(_k32.SetInformationJobObject(self.h, 9, ctypes.byref(info), ctypes.sizeof(info)))
        proc = _k32.OpenProcess(0x0101, False, pid)   # PROCESS_TERMINATE | PROCESS_SET_QUOTA
        # The game starts its first translator child seconds after start, long after this assignment.
        self.ok = ok and bool(proc) and bool(_k32.AssignProcessToJobObject(self.h, proc))
        if proc:
            _k32.CloseHandle(proc)

    def active(self) -> int:
        a = _Accounting()
        if not _k32.QueryInformationJobObject(self.h, 1, ctypes.byref(a), ctypes.sizeof(a), None):
            return 0
        return a.ActiveProcesses

    def kill(self) -> int:
        """Ends every process in the job; returns how many are still alive after up to 5 s."""
        _k32.TerminateJobObject(self.h, 1)
        for _ in range(50):
            if not self.active():
                break
            time.sleep(0.1)
        return self.active()

    def close(self) -> None:
        if self.h:
            _k32.CloseHandle(self.h)
            self.h = None


def commit_free_gb() -> float | None:
    if os.name != "nt":
        return None
    m = _MemStatus(dwLength=ctypes.sizeof(_MemStatus))
    return m.AvailPageFile / 2**30 if _k32.GlobalMemoryStatusEx(ctypes.byref(m)) else None


# ---- the instance registry

def norm(p: Path | str) -> str:
    """A path as instances compare it: absolute, junctions resolved, case folded on Windows."""
    s = os.path.realpath(str(p))
    return os.path.normcase(s)


def for_exe(p: Path) -> str:
    """A path as oot.exe takes it on the command line: relative to the repository (run.py starts it there)."""
    p = Path(os.path.abspath(p))
    if p.is_relative_to(ROOT):
        return p.relative_to(ROOT).as_posix()
    if HOST == "wsl":
        return subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True).stdout.strip()
    return str(p)


def read_info(d: Path) -> dict | None:
    try:
        return json.loads((d / "instance.json").read_text())
    except (OSError, ValueError):
        return None


def alive(d: Path, info: dict | None, game_pids: set[int]) -> bool:
    if held(d / "lock"):
        return True
    if info is None:
        # A folder without instance.json is being set up under the registry lock, or was left by a crash.
        try:
            return time.time() - d.stat().st_mtime < 60
        except OSError:
            return False
    if info.get("host") == HOST:
        return False
    # Started from the other side (WSL or Windows): its run.py's lock does not show here; trust the game and the clock.
    return info.get("pid") in game_pids or time.time() < info.get("started", 0) + 4 * info.get("seconds", 60) + 600


def live_instances(sweep: bool = False) -> list[tuple[Path, dict]]:
    """The live instances; with sweep, the folders of dead ones (a run.py that was killed) are deleted."""
    out = []
    if not INSTANCES.is_dir():
        return out
    pids = games()
    for d in sorted(p for p in INSTANCES.iterdir() if p.is_dir()):
        info = read_info(d)
        if alive(d, info, pids):
            if info:
                out.append((d, info))
        elif sweep:
            shutil.rmtree(d, ignore_errors=True)
    return out


def copy_profile(dest: Path) -> None:
    def skip(folder, names):
        return [n for n in names if n in PROFILE_SKIP] if norm(folder) == norm(PROFILE) else []

    if PROFILE.is_dir():
        shutil.copytree(PROFILE, dest, ignore=skip, dirs_exist_ok=True)
    else:
        dest.mkdir(parents=True, exist_ok=True)


class Instance:
    """One run's registration: a locked folder in build/logs/instances holding instance.json (what it runs and which
    paths it claims) and, unless --profile names one, the run's profile copy and its press-a script."""

    def __init__(self, name: str, claims: dict[str, Path], profile: Path | None, seconds: int, vram: float):
        self.name, self.seconds, self.vram = name, seconds, vram
        self.dir = INSTANCES / name
        self.claims = {k: norm(v) for k, v in claims.items()}
        self.own_profile = profile is None
        self.profile = self.dir / "profile" if profile is None else profile
        self.claims["profile"] = norm(self.profile)
        self.lockf = None
        self.info: dict = {}

    def open(self, kill_existing: bool, exe: Path, log: Path) -> None:
        reg = lock_wait(INSTANCES / "registry.lock")
        try:
            clash = []
            for d, info in live_instances(sweep=True):
                theirs = set((info.get("claims") or {}).values())
                same = (["name"] if d.name == self.name else []) + sorted(k for k, v in self.claims.items() if v in theirs)
                if same:
                    clash.append((d, info, same))
            for d, info, what in clash:
                desc = f"instance {d.name} (game pid {info.get('pid')}, {info.get('log')}) holds the same {', '.join(what)}"
                if not kill_existing:
                    sys.exit(f"run: {desc}; refusing to start (--kill-existing ends it)")
                print(f"run: {desc}; ending it", flush=True)
                end_instance(d, info)
            self.dir.mkdir(parents=True, exist_ok=True)
            f = open(self.dir / "lock", "a+b")
            if not try_lock(f):
                f.close()
                sys.exit(f"run: instance {self.name} is in use")
            self.lockf = f
            self.info = {"name": self.name, "host": HOST, "runner": os.getpid(), "pid": None, "exe": for_exe(exe),
                         "log": for_exe(log), "profile": for_exe(self.profile), "claims": self.claims,
                         "started": time.time(), "seconds": self.seconds, "vram": self.vram, "running": False}
            self.save()
        finally:
            unlock(reg)
        if self.own_profile:
            shutil.rmtree(self.profile, ignore_errors=True)
            copy_profile(self.profile)
        elif not self.profile.exists():
            print(f"run: new profile {for_exe(self.profile)}, seeded from build/profile", flush=True)
            copy_profile(self.profile)

    def save(self) -> None:
        tmp = self.dir / f"instance.json.{os.getpid()}"
        tmp.write_text(json.dumps(self.info, indent=1))
        os.replace(tmp, self.dir / "instance.json")

    def set(self, **kv) -> None:
        self.info.update(kv)
        self.save()

    def close(self) -> None:
        if self.lockf is None:   # never registered (the name or a path was taken): the folder is another run's
            return
        unlock(self.lockf)
        self.lockf = None
        shutil.rmtree(self.dir, ignore_errors=True)


def end_instance(d: Path, info: dict) -> None:
    """--kill-existing: ends another instance's run.py (so it does not retry) and its game, and waits for its lock."""
    runner = info.get("runner")
    if runner and info.get("host") == HOST and runner != os.getpid():
        try:
            os.kill(runner, signal.SIGTERM)
        except OSError:
            pass
    if info.get("pid"):
        kill_tree(info["pid"])
    for _ in range(100):
        if not held(d / "lock"):
            break
        time.sleep(0.1)
    shutil.rmtree(d, ignore_errors=True)


def admit(inst: Instance, alone: bool) -> list:
    """Waits until this run fits: no --alone run measuring; fewer than MAX_RUNS games; and the VRAM the running
    instances declared (--vram), plus LEVEL_GB for each game run.py did not start, leaves room for this one. With
    alone, holds the measured-run lock and waits until no game runs. Marks the instance running (its VRAM then counts
    for the others) and returns the files to unlock."""
    held_files = []
    quiet = INSTANCES / "alone.lock"
    if alone:
        held_files.append(lock_wait(quiet, "run: --alone: waiting for another --alone run to end", poll=2))
    # The lock is not seen across hosts (Windows Python's msvcrt lock and WSL's flock ignore each other; a WSL run once
    # went in beside a Windows --alone run), so the instance file says it too, and runs from both sides read that.
    inst.set(alone=alone)
    said = None
    while True:
        if not alone and held(quiet):
            why = "an --alone run (frame rates, GPU times) is measuring"
        else:
            reg = lock_wait(INSTANCES / "registry.lock")
            try:
                pids = games()
                others = [i for d, i in live_instances() if d.name != inst.name]
                running = [i for i in others if i.get("running")]
                strays = len(pids - {i.get("pid") for i in running})
                used = sum(float(i.get("vram", LEVEL_GB)) for i in running) + strays * LEVEL_GB
                count = len(running) + strays
                fits = False
                if not alone and any(i.get("alone") for i in others):
                    why = "an --alone run (frame rates, GPU times) is measuring or waiting to"
                elif alone:
                    fits, why = count == 0, f"{count} game(s) running; --alone waits for none"
                elif count >= MAX_RUNS:
                    fits, why = False, f"{count} games running, the limit is {MAX_RUNS} (OOT_MAX_RUNS)"
                else:
                    fits = count == 0 or used + inst.vram <= VRAM_BUDGET
                    why = (f"{used:.0f} GB of VRAM taken by {count} game(s) (a game run.py did not start counts "
                           f"{LEVEL_GB:.0f}); this run needs {inst.vram:.0f} of {VRAM_BUDGET:.0f} (--vram, OOT_VRAM_GB)")
                if fits:
                    inst.set(running=True)
                    return held_files
            finally:
                unlock(reg)
        if why != said:
            print(f"run: waiting: {why}", flush=True)
            said = why
        time.sleep(2)


def next_log() -> tuple[int, Path]:
    """The next run-<n>.log, created here so a concurrent run cannot take the same number."""
    LOGS.mkdir(parents=True, exist_ok=True)
    while True:
        nums = [int(m.group(1)) for p in LOGS.glob("run-*.log") if (m := re.fullmatch(r"run-(\d+)\.log", p.name))]
        n = max(nums, default=0) + 1
        log = LOGS / f"run-{n}.log"
        try:
            open(log, "x").close()
            return n, log
        except FileExistsError:
            continue


def wslenv(env: dict) -> str:
    names = [k for k in env if k.startswith("OOT_")]
    keep = [w for w in env.get("WSLENV", "").split(":") if w and w.split("/")[0] not in names]
    return ":".join(keep + [k + ("/p" if k in PATH_VARS else "") for k in sorted(names)])


def summary(log: Path) -> list[str]:
    text = log.read_text(errors="replace") if log.exists() else ""
    out = []
    pick = [r"vk caps: tensor cores.*", r"frametime: 60 s:.*", r"exception 0x[0-9a-f]+.*", r"FATAL.*",
            r"device lost.*", r"boot: frames \d+ presented.*", r"OOT_TENSOR_ONLY: program .*"]
    for pat in pick:
        hits = re.findall(r"^(?:\[host\] )?(" + pat + ")$", text, re.M)
        out += hits[:3] if pat.startswith("frametime") or pat.startswith("OOT_TENSOR") else hits[:1]
    for pat in (r"movie22: pacing:.*", r"perf: frame \d+: gpu avg [\d.]+ ms", r"run: other games running:.*[1-9].*"):
        hits = re.findall(r"(" + pat + ")", text)
        if hits:
            out.append(hits[-1])
    return out


def launch(args: list[str], child: dict, log_file, marker: str, inst: Instance, timeout: int) -> tuple[int, int]:
    """Runs the game to its end or the timeout, then ends it and its translator children. Returns (exit code,
    processes of this game left alive)."""
    proc = subprocess.Popen(args, cwd=ROOT, env=child, stdout=log_file, stderr=subprocess.STDOUT)
    job, pid = None, None
    try:
        if os.name == "nt":
            pid = proc.pid
            job = Job(pid)
            if not job.ok:
                print("run: cannot put the game in a job object; it is ended by PID", flush=True)
        else:   # WSL: proc.pid is a Linux pid; find the Windows one while the game starts
            for _ in range(60):
                if proc.poll() is not None or (pid := game_pid_by_marker(marker)):
                    break
                time.sleep(0.25)
        inst.set(pid=pid)
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            rc = -1
    finally:
        left = 0
        if job and job.ok:
            left = job.kill()
        elif pid:
            kill_tree(pid)
            left = int(pid in games())
        elif os.name != "nt" and (late := game_pid_by_marker(marker)):
            kill_tree(late)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        if job:
            job.close()
    return rc, left


def run_game(seconds: int = 60, shot: str | None = None, env: dict | None = None, press_a: bool = False,
             fresh_cache: str | None = None, dump: int | None = None, trace: str | None = None, volume: int = 0,
             focus_pin: bool = True, retry: int = 3, expect: list[str] | None = None, extra: list[str] | None = None,
             kill_existing: bool = False, quiet: bool = False, record: str | None = None, replay: str | None = None,
             skip_movies: bool = False, instance: str | None = None, profile: str | None = None,
             live_input: bool = False, alone: bool = False, vram: float | None = None) -> dict:
    """Runs the game once as its own instance (retrying the start-up crash); returns {ok, log, shot, exit, summary,
    missing, run, instance, others}."""
    if not EXE.exists():
        sys.exit(f"run: {EXE} is not built (tools/build_game.py)")
    if instance and not re.fullmatch(r"[A-Za-z0-9_.-]+", instance):
        sys.exit(f"run: --instance {instance}: letters, digits, '_', '.' and '-' only")
    if replay and not (ROOT / replay).exists() and not Path(replay).exists():
        sys.exit(f"run: no input script {replay}")
    env = {k: str(v) for k, v in (env or {}).items()}
    n, log = next_log()
    name = instance or f"run-{n}"
    shot_path = (Path(shot) if Path(shot).is_absolute() else ROOT / shot) if shot else None
    cache = LOGS / f"sc-{fresh_cache}" if fresh_cache else None
    claims = {k: v for k, v in (("shot", shot_path), ("fresh-cache", cache),
                                ("record", ROOT / record if record else None)) if v}
    prof = (Path(profile) if Path(profile).is_absolute() else ROOT / profile) if profile else None
    if vram is None:   # a replay goes to the level (build/logs/a-spam.txt); the title alone takes little
        vram = LEVEL_GB if replay else TITLE_GB
    inst = Instance(name, claims, prof, seconds, vram)
    INSTANCES.mkdir(parents=True, exist_ok=True)
    locks = []
    try:
        try:
            inst.open(kill_existing, EXE, log)
        except SystemExit:
            log.unlink(missing_ok=True)   # the number stays free for the next run
            raise
        locks = admit(inst, alone)
        free = commit_free_gb()
        if free is not None and free < 24 and vram >= LEVEL_GB and not quiet:
            print(f"run: {free:.0f} GB of commit left; a game commits about 24 GB at the level (Windows grows its "
                  "page file, slowly, or the run fails)", flush=True)
        child = dict(os.environ)
        child["OOT_VOLUME"] = str(volume)
        child["OOT_BACKGROUND_WINDOW"] = "1"
        if focus_pin:
            child["OOT_ASSUME_FOCUS"] = "1"
        if not (live_input or record or "OOT_TEST_PAD" in env or "OOT_TEST_PAD" in child):
            child["OOT_INPUT"] = "none"
        if trace:
            child["OOT_TRACE"] = trace
        if press_a:
            script = inst.dir / "press-a.txt"
            script.write_text("600 A 10\n")
            child["OOT_INPUT_SCRIPT"] = for_exe(script)
        if record:
            child["OOT_INPUT_RECORD"] = record
        if replay:
            child["OOT_INPUT_SCRIPT"] = replay
        if skip_movies:
            child["OOT_MOVIE_SKIP"] = "2"
        child.update(env)
        result = {"ok": False}
        for attempt in range(1, max(1, retry) + 1):
            if attempt > 1:
                n, log = next_log()
                inst.set(log=for_exe(log))
            if cache:
                shutil.rmtree(cache, ignore_errors=True)
                cache.mkdir(parents=True)
                child["OOT_SHADER_CACHE"] = for_exe(cache)
            if dump is not None:
                child["OOT_DUMP"] = str(dump)
                child["OOT_DUMP_DIR"] = f"build/logs/dump-{n}"
            child["WSLENV"] = wslenv(child)
            if shot_path:
                shot_path.unlink(missing_ok=True)
            exe_arg = str(EXE) if os.name == "nt" else for_exe(EXE)
            marker = for_exe(inst.profile)
            args = [exe_arg, "--profile", marker]
            # A mode of its own (--probe-tensor, --selftest, ...) after "--" excludes --seconds: oot.exe's usage
            # takes one mode, and --seconds is the play mode. The timeout still follows --seconds.
            if not any(a.startswith("--probe") or a in ("--selftest", "--boot", "--prepare-shaders", "--warm-shaders")
                       for a in extra or []):
                args += ["--seconds", str(seconds)]
            if shot_path:
                args += ["--shot", for_exe(shot_path)]
            args += extra or []
            if not quiet:
                print(f"run: run-{n} (instance {name}, attempt {attempt}), {seconds} s" + (f", shot {shot}" if shot else ""),
                      flush=True)
            others_before = len(games())
            t0 = time.monotonic()
            with open(log, "w") as f:
                f.write(f"run: instance {name}, profile {marker}, {for_exe(EXE)}\n")
                f.flush()
                rc, left = launch(args, child, f, marker, inst, seconds + 240)
            took = time.monotonic() - t0
            others_after = len(games())
            with open(log, "a") as f:
                f.write(f"exit {rc}\nprocesses left: {left}\n"
                        f"run: other games running: {others_before} at start, {others_after} at the end\n")
            # The start-up crash comes within the first frames; a crash later is a result, not a reason to run again.
            early = rc != 0 and took < min(seconds, 25)
            missing_shot = bool(shot_path) and not shot_path.exists()
            result = {"ok": not early and not missing_shot and rc in (0,), "log": str(log), "exit": rc,
                      "shot": str(shot_path) if shot_path and shot_path.exists() else None, "seconds_run": round(took),
                      "summary": summary(log), "run": n, "instance": name, "others": max(others_before, others_after)}
            if result["ok"] or not early:
                break
            if not quiet:
                print(f"run: run-{n} ended early (exit {rc} after {took:.0f} s{', no shot' if missing_shot else ''}); "
                      "retrying", flush=True)
    finally:
        for f in locks:
            unlock(f)
        inst.close()
    text = Path(result["log"]).read_text(errors="replace")
    result["missing"] = [p for p in expect or [] if not re.search(p, text, re.M)]
    if result["missing"]:
        result["ok"] = False
    return result


def list_instances() -> int:
    live = live_instances()
    mine = set()
    for d, info in live:
        age = time.time() - info.get("started", time.time())
        mine.add(info.get("pid"))
        pid = info.get("pid") or ("starting" if info.get("running") else "waiting")
        print(f"{d.name:20} game pid {pid:>8}  {info.get('vram', '?')} GB  {age:5.0f} s of {info.get('seconds')}  {info.get('exe')}  "
              f"{info.get('log')}  profile {info.get('profile')}  ({info.get('host')})")
    stray = games() - mine
    if not live:
        print("run: no instances running")
    if stray:
        print(f"run: {len(stray)} other oot.exe not started by this run.py (by hand, or an older run.py): "
              f"pid {', '.join(map(str, sorted(stray)))}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[2:]))
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--exe", help="oot.exe of another build dir (relative to the repo), e.g. build/v131072-t2/oot.exe "
                                  "from tools/build_game.py --out: its own shader and pipeline caches and translator")
    ap.add_argument("--shot", help="PNG path (relative to the repo) for a screenshot at the end; builds pipelines "
                                   "synchronously, so the frame does not depend on build timing")
    ap.add_argument("--press-a", action="store_true", help="press A at frame 600 (the title menu)")
    ap.add_argument("--env", action="append", default=[], metavar="OOT_X=V")
    ap.add_argument("--fresh-cache", metavar="NAME")
    ap.add_argument("--dump", type=int, metavar="FRAME", help="frame dump into build/logs/dump-<run>/")
    ap.add_argument("--trace", help="OOT_TRACE value (gfx, perf, ...)")
    ap.add_argument("--volume", type=int, default=0)
    ap.add_argument("--no-focus-pin", action="store_true")
    ap.add_argument("--retry", type=int, default=3)
    ap.add_argument("--expect", action="append", default=[], metavar="REGEX")
    ap.add_argument("--kill-existing", action="store_true",
                    help="end a live instance that holds this run's name, profile, cache, record file or shot path")
    ap.add_argument("--record", metavar="FILE", help="write the player's input as a script (relative to the repo)")
    ap.add_argument("--replay", metavar="FILE", help="play an input script back (from --record or by hand)")
    ap.add_argument("--skip-movies", action="store_true", help="end every movie after the title loop at once")
    ap.add_argument("--instance", metavar="NAME", help="name of this run's instance (default run-<n>)")
    ap.add_argument("--profile", metavar="DIR", help="keep this run's saves in DIR (relative to the repo; seeded from "
                                                     "build/profile when new) instead of a copy deleted afterwards")
    ap.add_argument("--live-input", action="store_true", help="read the real gamepad and keyboard")
    ap.add_argument("--alone", action="store_true", help="wait until no other game runs and hold new runs back until "
                                                         "this one ends (frame rates, GPU times)")
    ap.add_argument("--vram", type=float, metavar="GB", help=f"VRAM this run needs, for admission (default "
                                                              f"{LEVEL_GB:.0f} with --replay, else {TITLE_GB:.0f})")
    ap.add_argument("--list", action="store_true", help="list the live instances and exit")
    ap.add_argument("extra", nargs="*", help="arguments for oot.exe after --")
    a = ap.parse_args()
    if a.list:
        return list_instances()
    if os.name != "nt":
        # WSL has no job object to end the game with run.py: SIGTERM and SIGHUP (a closed terminal) unwind through
        # run_game's cleanup like Ctrl+C does. SIGKILL cannot be caught; tools/run.py --list then shows the game as a
        # stray, and taskkill.exe /T /PID ends it.
        def leave(sig, _frame):
            raise SystemExit(128 + sig)
        signal.signal(signal.SIGTERM, leave)
        signal.signal(signal.SIGHUP, leave)
    if a.exe:
        global EXE
        EXE = ROOT / a.exe
    env = {}
    for kv in a.env:
        k, _, v = kv.partition("=")
        if not k.startswith("OOT_"):
            sys.exit(f"run: --env {kv}: only OOT_* variables reach the game")
        env[k] = v
    r = run_game(a.seconds, a.shot, env, a.press_a, a.fresh_cache, a.dump, a.trace, a.volume, not a.no_focus_pin,
                 a.retry, a.expect, a.extra, a.kill_existing, record=a.record, replay=a.replay,
                 skip_movies=a.skip_movies, instance=a.instance, profile=a.profile, live_input=a.live_input,
                 alone=a.alone, vram=a.vram)
    print(f"run: {'ok' if r['ok'] else 'FAILED'}: {Path(r['log']).relative_to(ROOT)}, exit {r['exit']}, "
          f"{r.get('seconds_run')} s" + (f", shot {r['shot']}" if r.get("shot") else ""))
    for line in r["summary"]:
        print("  " + line[:200])
    for p in r["missing"]:
        print(f"  EXPECTED BUT MISSING: {p}")
    return 0 if r["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
