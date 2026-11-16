#!/usr/bin/env python3
"""End-to-end correctness verification for Parsec.

Builds every target and runs, in order:

  1. build      configure and build all targets with warnings as errors
  2. ctest      every registered test (unit, protocol over MPI and TCP, database checks, aggregates)
  3. matrix     exp_1..exp_8 in check mode under the paper configurations, every BMT method, single-threaded and
                small-batch execution, several background queues, and the TCP backend
  4. figure4    the JIT and background microbenchmarks of Figure 4 on small inputs
  5. sanitizers (with --sanitizers) unit and protocol tests in an AddressSanitizer/UBSan build

Each check prints one line, "name ........ OK" (or FAIL / SKIP), followed by a summary. Logs of failed checks are
kept under <build-dir>/verify-logs. The exit code is 0 only when nothing failed.

Examples:
  tools/verify.py                       # everything except sanitizers
  tools/verify.py --quick               # ctest plus the paper configurations
  tools/verify.py --skip-build --only matrix
  CC=gcc CXX=g++ tools/verify.py --sanitizers --cmake-arg=-DCMAKE_PREFIX_PATH=/opt/gtest
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from typing import Callable, List, Optional, Sequence, Tuple

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "database"))
import verify_exp  # noqa: E402  (expected results and output parsing of the experiment checks)

WIDTH = 72
STAGES = ["build", "ctest", "matrix", "figure4", "sanitizers"]

# Name, extra experiment arguments. The first three are the configurations the paper evaluates
# (artifact/experiments.yaml on the artifact branch).
PAPER_CONFIGS = [
    ("parsec", ["--baseline_mode=false", "--no_compaction=false", "--batch_size=256"]),
    ("parsec_base", ["--baseline_mode=true", "--no_compaction=true", "--batch_size=0"]),
    ("parsec_noncompact", ["--baseline_mode=false", "--no_compaction=true", "--batch_size=256"]),
]
BACKGROUND = ["--bmt_method=bmt_background", "--disable_arith=false", "--max_bmts=10000"]
EXTRA_CONFIGS = [
    ("bmt_fixed", ["--bmt_method=bmt_fixed"]),
    ("bmt_background", BACKGROUND),
    ("bmt_background single-thread", BACKGROUND + ["--disable_multi_thread=true"]),
    ("bmt_background 2 queues", BACKGROUND + ["--bmt_queue_num=2"]),
    ("single-thread", ["--disable_multi_thread=true"]),
    ("one batch", ["--batch_size=0"]),
    ("batch size 1", ["--batch_size=1"]),
    ("batch size 4", ["--batch_size=4"]),
]
FIGURE4_PRIMITIVES = [("!=", "not equal"), ("<", "less"), ("==", "equal"), ("mux", "mux"), ("sort", "sort")]


class Reporter:
    def __init__(self, log_dir: str, color: bool):
        self.log_dir = log_dir
        self.color = color
        self.results: List[Tuple[str, str, str]] = []  # (name, status, log path)

    def _paint(self, status: str) -> str:
        if not self.color:
            return status
        code = {"OK": "32", "FAIL": "31", "SKIP": "33"}.get(status, "0")
        return f"\033[{code}m{status}\033[0m"

    def section(self, title: str) -> None:
        print(f"\n== {title}", flush=True)

    def record(self, name: str, status: str, output: str = "", seconds: Optional[float] = None) -> None:
        log = ""
        if status == "FAIL" and output:
            os.makedirs(self.log_dir, exist_ok=True)
            log = os.path.join(self.log_dir, re.sub(r"[^A-Za-z0-9_.-]+", "_", name) + ".log")
            with open(log, "w") as f:
                f.write(output)
        self.results.append((name, status, log))
        timing = f" ({seconds:.1f}s)" if seconds is not None and seconds >= 1 else ""
        dots = "." * max(3, WIDTH - len(name) - len(timing))
        print(f"{name}{timing} {dots} {self._paint(status)}", flush=True)

    def summary(self) -> int:
        counts = {s: sum(1 for _, st, _ in self.results if st == s) for s in ("OK", "FAIL", "SKIP")}
        print(f"\n{counts['OK']} OK, {counts['FAIL']} FAIL, {counts['SKIP']} SKIP")
        for name, status, log in self.results:
            if status == "FAIL":
                print(f"  FAIL {name}" + (f"  -> {log}" if log else ""))
        return 1 if counts["FAIL"] else 0


def run(cmd: Sequence[str], timeout: float, cwd: Optional[str] = None,
        env: Optional[dict] = None) -> Tuple[int, str, float]:
    start = time.time()
    try:
        p = subprocess.run(list(cmd), cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout, time.time() - start
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return -1, out + f"\n[verify] timed out after {timeout:.0f}s\n", time.time() - start


def mpirun_command(args: argparse.Namespace) -> List[str]:
    flags = shlex.split(args.mpirun_flags) if args.mpirun_flags is not None else []
    if args.mpirun_flags is None:
        _, version, _ = run([args.mpirun, "--version"], 30)
        if "Open MPI" in version or "OpenRTE" in version:
            flags = ["--oversubscribe"]
    return [args.mpirun, *flags, "-np", "3"]


# ---------------------------------------------------------------------------------------------------------------------
# Stages


def stage_build(args, rep: Reporter, build_dir: str, extra_cmake: List[str], name: str = "build") -> bool:
    configure = ["cmake", "-S", REPO, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DPARSEC_BUILD_DATABASE=ON", "-DPARSEC_BUILD_BENCHMARKS=ON", "-DPARSEC_BUILD_EXPERIMENTS=ON",
                 "-DPARSEC_BUILD_TESTS=ON", "-DPARSEC_WARNINGS_AS_ERRORS=ON", *args.cmake_arg, *extra_cmake]
    code, out, secs = run(configure, 600)
    if code != 0:
        rep.record(f"{name}: configure", "FAIL", out, secs)
        return False
    rep.record(f"{name}: configure", "OK", seconds=secs)
    if "GoogleTest not found" in out:
        rep.record(f"{name}: GoogleTest found (unit tests)", "SKIP")
    code, out2, secs = run(["cmake", "--build", build_dir, "--parallel", str(args.build_jobs)], 3600)
    rep.record(f"{name}: compile all targets", "OK" if code == 0 else "FAIL", out2, secs)
    return code == 0


def display_test_name(test: str) -> str:
    """unit.Crypto.X -> 'unit  Crypto.X'; correctness.tcp.bool_less -> 'protocol  bool_less [tcp]'; ..."""
    if test.startswith("unit."):
        return "unit  " + test[len("unit."):]
    if test.startswith("database."):
        parts = test.split(".")[1:]
        return "database  " + parts[0] + (" [" + ", ".join(parts[1:]) + "]" if len(parts) > 1 else "")
    if test.startswith("correctness."):
        rest = test[len("correctness."):]
        backend = "mpi"
        if rest.startswith("tcp."):
            backend, rest = "tcp", rest[len("tcp."):]
        return f"protocol  {rest.replace('_', ' ')} [{backend}]"
    return test


def stage_ctest(args, rep: Reporter, build_dir: str, prefix: str = "", labels: Optional[str] = None,
                env: Optional[dict] = None) -> None:
    junit = os.path.join(build_dir, "verify-ctest.xml")
    cmd = ["ctest", "--test-dir", build_dir, "-j", str(args.jobs), "--output-junit", junit, "--timeout", "600"]
    if labels:
        cmd += ["-L", labels]
    code, out, _ = run(cmd, 7200, env=env)
    if not os.path.exists(junit):
        rep.record(f"{prefix}ctest", "FAIL", out)
        return
    group = {"unit": 0, "protocol": 1, "database": 2}
    cases = list(ET.parse(junit).getroot().iter("testcase"))
    cases.sort(key=lambda c: (group.get(display_test_name(c.get("name", "")).split()[0], 3),
                              display_test_name(c.get("name", ""))))
    for case in cases:
        name = case.get("name", "?")
        status = "OK"
        if case.find("failure") is not None or case.get("status") == "fail":
            status = "FAIL"
        elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
            status = "SKIP"
        output = "".join(case.find("system-out").itertext()) if case.find("system-out") is not None else ""
        rep.record(prefix + display_test_name(name), status, output, float(case.get("time", "0") or 0))
    os.remove(junit)


def check_experiment(args, exp_dir: str, exp: int, extra: List[str], comm: str) -> Tuple[str, str, float]:
    exe = os.path.join(exp_dir, f"exp_{exp}")
    if comm == "mpi":
        code, out, secs = run([*mpirun_command(args), exe, "--check=true", *extra], args.timeout)
    else:
        # verify_exp.py starts the three TCP ranks; it accepts only the BMT method.
        method = next((a.split("=", 1)[1] for a in extra if a.startswith("--bmt_method=")), "bmt_jit")
        cmd = [sys.executable, os.path.join(REPO, "tests", "database", "verify_exp.py"), "--exp", str(exp),
               "--comm", "tcp", "--bin-dir", exp_dir, "--bmt-method", method,
               "--tcp-base-port", str(23000 + 100 * exp), "--timeout", str(args.timeout)]
        if method == "bmt_background":
            cmd += ["--max-bmts", "10000"]
        code, out, secs = run(cmd, args.timeout * 2)
        return ("OK" if code == 0 and "PASS" in out else "FAIL"), out, secs
    if code != 0:
        return "FAIL", out + f"\n[verify] exit code {code}\n", secs
    try:
        if exp == 7:
            ok = verify_exp.parse_scalar(out) == 44
        else:
            ok = sorted(set(verify_exp.parse_rows(out))) == sorted(set(verify_exp.expected_rows(exp)))
    except Exception as e:  # missing or malformed check output
        return "FAIL", out + f"\n[verify] {e}\n", secs
    return ("OK" if ok else "FAIL"), out, secs


def stage_matrix(args, rep: Reporter, build_dir: str) -> None:
    exp_dir = os.path.join(build_dir, "experiments", "database")
    configs = PAPER_CONFIGS if args.quick else PAPER_CONFIGS + EXTRA_CONFIGS
    jobs: List[Tuple[str, Callable[[], Tuple[str, str, float]]]] = []
    for cname, extra in configs:
        for exp in range(1, 9):
            jobs.append((f"exp_{exp}  {cname} [mpi]",
                         lambda e=exp, x=extra: check_experiment(args, exp_dir, e, x, "mpi")))
    if not args.quick:
        for cname, extra in [("bmt_jit", []), ("bmt_background", BACKGROUND)]:
            for exp in range(1, 9):
                jobs.append((f"exp_{exp}  {cname} [tcp]",
                             lambda e=exp, x=extra: check_experiment(args, exp_dir, e, x, "tcp")))
    # Each check starts three processes; run a few at a time and report in the original order.
    workers = max(1, args.jobs // 2)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [(name, pool.submit(fn)) for name, fn in jobs]
        for name, fut in futures:
            status, out, secs = fut.result()
            rep.record(name, status, out, secs)


def stage_figure4(args, rep: Reporter, build_dir: str) -> None:
    bench_dir = os.path.join(build_dir, "benchmarks", "primitives")
    for prim, label in FIGURE4_PRIMITIVES:
        num = 2000 if prim == "sort" else 20000
        for kind in ("jit", "background"):
            exe = os.path.join(bench_dir, f"benchmark_{kind}_single")
            code, out, secs = run([*mpirun_command(args), exe, f"--primitive={prim}", f"--num={num}", "--width=64",
                                   "--batch_size=256"], args.timeout * 2)
            ok = code == 0 and "RESULT:" in out and "Test failed" not in out
            rep.record(f"figure4  {label} [{kind}]", "OK" if ok else "FAIL", out, secs)


def stage_sanitizers(args, rep: Reporter) -> None:
    san_dir = os.path.join(args.build_dir + "-sanitizers")
    configure = ["cmake", "-S", REPO, "-B", san_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
                 "-DPARSEC_ENABLE_SANITIZERS=ON", "-DPARSEC_BUILD_DATABASE=OFF", "-DPARSEC_BUILD_BENCHMARKS=OFF",
                 "-DPARSEC_BUILD_EXPERIMENTS=OFF", "-DPARSEC_BUILD_TESTS=ON", "-DPARSEC_WARNINGS_AS_ERRORS=ON",
                 *args.cmake_arg]
    code, out, secs = run(configure, 600)
    if code != 0:
        rep.record("sanitizers: configure", "FAIL", out, secs)
        return
    code, out, secs = run(["cmake", "--build", san_dir, "--parallel", str(args.build_jobs)], 3600)
    rep.record("sanitizers: compile", "OK" if code == 0 else "FAIL", out, secs)
    if code != 0:
        return
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    stage_ctest(args, rep, san_dir, prefix="asan  ", env=env)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "verify"))
    ap.add_argument("--skip-build", action="store_true", help="reuse an existing build in --build-dir")
    ap.add_argument("--quick", action="store_true", help="ctest plus the paper configurations only")
    ap.add_argument("--sanitizers", action="store_true", help="also run unit and protocol tests under ASan/UBSan")
    ap.add_argument("--only", choices=STAGES, action="append", help="run only these stages (repeatable)")
    ap.add_argument("--jobs", type=int, default=4, help="parallel test jobs (default 4)")
    ap.add_argument("--build-jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=180, help="seconds per experiment run")
    ap.add_argument("--mpirun", default="mpirun")
    ap.add_argument("--mpirun-flags", default=None, help="launcher flags; default --oversubscribe for Open MPI")
    ap.add_argument("--cmake-arg", action="append", default=[], help="extra CMake argument (repeatable)")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)

    stages = args.only or [s for s in STAGES if s != "sanitizers" or args.sanitizers]
    if args.skip_build and "build" in stages:
        stages.remove("build")
    for tool in ("cmake", "ninja", args.mpirun):
        if shutil.which(tool) is None:
            print(f"required tool not found: {tool}")
            return 2

    rep = Reporter(os.path.join(args.build_dir, "verify-logs"), sys.stdout.isatty() and not args.no_color)
    shutil.rmtree(rep.log_dir, ignore_errors=True)
    print(f"Parsec verification  (build dir: {args.build_dir})")
    start = time.time()

    if "build" in stages:
        rep.section("Build")
        if not stage_build(args, rep, args.build_dir, []):
            return rep.summary()
    if "ctest" in stages:
        rep.section("Registered tests (unit, protocol, database)")
        stage_ctest(args, rep, args.build_dir)
    if "matrix" in stages:
        rep.section("Experiment checks across configurations")
        stage_matrix(args, rep, args.build_dir)
    if "figure4" in stages and not args.quick:
        rep.section("Figure 4 microbenchmarks (JIT vs background)")
        stage_figure4(args, rep, args.build_dir)
    if "sanitizers" in stages:
        rep.section("AddressSanitizer / UBSan")
        stage_sanitizers(args, rep)

    code = rep.summary()
    print(f"total time {time.time() - start:.0f}s")
    return code


if __name__ == "__main__":
    sys.exit(main())
