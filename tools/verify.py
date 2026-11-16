#!/usr/bin/env python3
"""End-to-end correctness verification for Parsec.

Runs every correctness check, layer by layer from the bottom up, so that the first failing layer points at the cause:

   1. Build                  all targets, warnings as errors
   2. Unit tests             single-process: crypto, math, SIMD, strings
   3. Communication          point-to-point and server-to-server messaging over MPI and TCP
   4. Oblivious transfer     base OT and IKNP OT extension
   5. BMT generation         Beaver triples from OT
   6. Boolean operators      XOR, AND, equality, less-than, mux              (JIT, fixed and background BMTs; TCP)
   7. Arithmetic operators   add, multiply, less-than, mux                   (same variants)
   8. Share conversion       A2B and B2A                                     (same variants)
   9. Secret containers      ArithSecret/BoolSecret/BitSecret and Secrets::sort
  10. Database operators     filters, sorts, grouping and aggregates, distinct, joins, IN, each against plaintext in
                             four execution modes
  11. Queries                exp_1..exp_8 in check mode: the registered runs, then the paper configurations
                             (parsec, parsec_base, parsec_noncompact) and further BMT, threading and batch settings
  12. Benchmarks             the Figure 4 JIT and background microbenchmarks on small inputs
  13. Sanitizers             (with --sanitizers) layers 2-9 under AddressSanitizer and UBSan

Layers 2-11 run the tests registered with CTest (named <layer>.<group>.<case>.<backend>[.<bmt>]); the extra query
configurations and the benchmarks are run by this script. Each check prints one line, "name ........ OK" (or FAIL or
SKIP); a summary follows, and logs of failed checks are kept under <build-dir>/verify-logs. The exit code is 0 only
when nothing failed.

Examples:
  tools/verify.py                         # everything except sanitizers, about 5 minutes on 12 cores
  tools/verify.py --quick                 # registered tests only
  tools/verify.py --sanitizers
  tools/verify.py --skip-build --build-dir build/dev --only queries
  CC=gcc CXX=g++ tools/verify.py --cmake-arg=-DCMAKE_PREFIX_PATH=/opt/googletest
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
from typing import Dict, List, Optional, Sequence, Tuple

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "database"))
import verify_exp  # noqa: E402  (expected results and output parsing of the query checks)

WIDTH = 76

# Sections in dependency order: (key for --only, title, prefix of the CTest names it contains).
LAYERS = [
    ("unit", "Unit tests", "unit."),
    ("comm", "Communication", "protocol.comm."),
    ("ot", "Oblivious transfer", "protocol.ot."),
    ("bmt", "BMT generation", "protocol.bmt."),
    ("boolean", "Boolean operators", "protocol.boolean."),
    ("arithmetic", "Arithmetic operators", "protocol.arithmetic."),
    ("conversion", "Share conversion", "protocol.conversion."),
    ("secret", "Secret containers and sorting", "protocol.secret."),
    ("db", "Database operators", "db."),
    ("queries", "Queries (exp_1..exp_8)", "query."),
]
STAGES = ["build"] + [key for key, _, _ in LAYERS] + ["benchmarks", "sanitizers"]

# Query configurations beyond the registered runs (which cover JIT and background BMTs over MPI and JIT over TCP).
# The first three are the configurations the paper evaluates (artifact/experiments.yaml on the artifact branch).
BACKGROUND = ["--bmt_method=bmt_background", "--disable_arith=false", "--max_bmts=10000"]
QUERY_CONFIGS = [
    ("parsec", "mpi", ["--baseline_mode=false", "--no_compaction=false", "--batch_size=256"]),
    ("parsec_base", "mpi", ["--baseline_mode=true", "--no_compaction=true", "--batch_size=0"]),
    ("parsec_noncompact", "mpi", ["--baseline_mode=false", "--no_compaction=true", "--batch_size=256"]),
    ("fixed", "mpi", ["--bmt_method=bmt_fixed"]),
    ("background, single-thread", "mpi", BACKGROUND + ["--disable_multi_thread=true"]),
    ("background, 2 queues", "mpi", BACKGROUND + ["--bmt_queue_num=2"]),
    ("single-thread", "mpi", ["--disable_multi_thread=true"]),
    ("one batch", "mpi", ["--batch_size=0"]),
    ("batch size 1", "mpi", ["--batch_size=1"]),
    ("batch size 4", "mpi", ["--batch_size=4"]),
    ("background", "tcp", BACKGROUND),
]
FIGURE4_PRIMITIVES = [("!=", "not equal"), ("<", "less"), ("==", "equal"), ("mux", "mux"), ("sort", "sort")]


class Reporter:
    def __init__(self, log_dir: str, color: bool):
        self.log_dir = log_dir
        self.color = color
        self.results: List[Tuple[str, str, str, str]] = []  # (section, name, status, log path)
        self.section_name = ""

    def _paint(self, status: str) -> str:
        if not self.color:
            return status
        code = {"OK": "32", "FAIL": "31", "SKIP": "33"}.get(status, "0")
        return f"\033[{code}m{status}\033[0m"

    def section(self, number: int, title: str) -> None:
        self.section_name = title
        print(f"\n[{number:2d}] {title}", flush=True)

    def record(self, name: str, status: str, output: str = "", seconds: Optional[float] = None) -> None:
        log = ""
        if status == "FAIL" and output:
            os.makedirs(self.log_dir, exist_ok=True)
            log = os.path.join(self.log_dir, re.sub(r"[^A-Za-z0-9_.-]+", "_", f"{self.section_name}-{name}") + ".log")
            with open(log, "w") as f:
                f.write(output)
        self.results.append((self.section_name, name, status, log))
        timing = f" ({seconds:.1f}s)" if seconds is not None and seconds >= 1 else ""
        label = f"     {name}{timing} "
        print(f"{label}{'.' * max(3, WIDTH - len(label))} {self._paint(status)}", flush=True)

    def summary(self) -> int:
        print("\nSummary")
        sections: Dict[str, List[str]] = {}
        for section, _, status, _ in self.results:
            sections.setdefault(section, []).append(status)
        for section, statuses in sections.items():
            ok, fail, skip = (statuses.count(s) for s in ("OK", "FAIL", "SKIP"))
            state = "FAIL" if fail else ("SKIP" if ok == 0 else "OK")
            detail = f"{ok}/{len(statuses)}" + (f", {skip} skipped" if skip else "")
            label = f"     {section} ({detail}) "
            print(f"{label}{'.' * max(3, WIDTH - len(label))} {self._paint(state)}")
        counts = {s: sum(1 for r in self.results if r[2] == s) for s in ("OK", "FAIL", "SKIP")}
        print(f"\n{counts['OK']} OK, {counts['FAIL']} FAIL, {counts['SKIP']} SKIP")
        for section, name, status, log in self.results:
            if status == "FAIL":
                print(f"  FAIL {section}: {name}" + (f"\n       log: {log}" if log else ""))
        return 1 if counts["FAIL"] else 0


def run(cmd: Sequence[str], timeout: float, env: Optional[dict] = None) -> Tuple[int, str, float]:
    start = time.time()
    try:
        p = subprocess.run(list(cmd), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout, time.time() - start
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return -1, out + f"\n[verify] timed out after {timeout:.0f}s\n", time.time() - start


def mpirun_command(args: argparse.Namespace) -> List[str]:
    if args.mpirun_flags is not None:
        return [args.mpirun, *shlex.split(args.mpirun_flags), "-np", "3"]
    _, version, _ = run([args.mpirun, "--version"], 30)
    flags = ["--oversubscribe"] if ("Open MPI" in version or "OpenRTE" in version) else []
    return [args.mpirun, *flags, "-np", "3"]


# ---------------------------------------------------------------------------------------------------------------------
# Build and CTest


def configure_and_build(args, rep: Reporter, build_dir: str, cmake_args: List[str]) -> bool:
    code, out, secs = run(["cmake", "-S", REPO, "-B", build_dir, "-G", "Ninja", *cmake_args, *args.cmake_arg], 600)
    rep.record("configure", "OK" if code == 0 else "FAIL", out, secs)
    if code != 0:
        return False
    if "GoogleTest not found" in out:
        rep.record("GoogleTest (unit tests are skipped without it)", "SKIP")
    code, out, secs = run(["cmake", "--build", build_dir, "--parallel", str(args.build_jobs)], 3600)
    rep.record("compile all targets", "OK" if code == 0 else "FAIL", out, secs)
    return code == 0


def run_ctest(args, build_dir: str, env: Optional[dict] = None) -> Tuple[Dict[str, Tuple[str, str, float]], str]:
    """Runs every registered test once; returns name -> (status, output, seconds)."""
    junit = os.path.join(build_dir, "verify-ctest.xml")
    if os.path.exists(junit):
        os.remove(junit)
    _, out, _ = run(["ctest", "--test-dir", build_dir, "-j", str(args.jobs), "--output-junit", junit,
                     "--timeout", "900"], 7200, env=env)
    results: Dict[str, Tuple[str, str, float]] = {}
    if os.path.exists(junit):
        for case in ET.parse(junit).getroot().iter("testcase"):
            status = "OK"
            if case.find("failure") is not None or case.get("status") == "fail":
                status = "FAIL"
            elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
                status = "SKIP"
            sysout = case.find("system-out")
            output = "".join(sysout.itertext()) if sysout is not None else ""
            results[case.get("name", "?")] = (status, output, float(case.get("time", "0") or 0))
        os.remove(junit)
    return results, out


def display_name(test: str, prefix: str) -> str:
    """protocol.boolean.and_xor.mpi.background -> 'and_xor [mpi, background]'; unit.Suite.Case stays Suite.Case."""
    rest = test[len(prefix):]
    if prefix == "unit.":
        return rest
    parts = rest.split(".")
    variants = [p for p in parts[1:]]
    return parts[0].replace("_", " ") + (f" [{', '.join(variants)}]" if variants else "")


def report_layer(rep: Reporter, results: Dict[str, Tuple[str, str, float]], prefix: str) -> int:
    names = sorted(n for n in results if n.startswith(prefix))
    for name in names:
        status, output, secs = results[name]
        rep.record(display_name(name, prefix), status, output, secs)
    return len(names)


# ---------------------------------------------------------------------------------------------------------------------
# Query configurations and benchmarks


def check_query(args, exp_dir: str, exp: int, extra: List[str], comm: str) -> Tuple[str, str, float]:
    exe = os.path.join(exp_dir, f"exp_{exp}")
    if comm == "tcp":
        # verify_exp.py starts the three TCP ranks; it takes the BMT method only.
        method = next((a.split("=", 1)[1] for a in extra if a.startswith("--bmt_method=")), "bmt_jit")
        cmd = [sys.executable, os.path.join(REPO, "tests", "database", "verify_exp.py"), "--exp", str(exp),
               "--comm", "tcp", "--bin-dir", exp_dir, "--bmt-method", method,
               "--tcp-base-port", str(26000 + 100 * exp), "--timeout", str(args.timeout)]
        if method == "bmt_background":
            cmd += ["--max-bmts", "10000"]
        code, out, secs = run(cmd, args.timeout * 2)
        return ("OK" if code == 0 and "PASS" in out else "FAIL"), out, secs
    code, out, secs = run([*mpirun_command(args), exe, "--check=true", *extra], args.timeout)
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


def run_query_configs(args, rep: Reporter, build_dir: str) -> None:
    exp_dir = os.path.join(build_dir, "experiments", "database")
    jobs = [(f"exp_{exp} [{comm}, {name}]", exp, extra, comm)
            for name, comm, extra in QUERY_CONFIGS for exp in range(1, 9)]
    # Each run starts three processes; run a few at a time and report in order.
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs // 2)) as pool:
        futures = [(label, pool.submit(check_query, args, exp_dir, exp, extra, comm))
                   for label, exp, extra, comm in jobs]
        for label, fut in futures:
            status, out, secs = fut.result()
            rep.record(label, status, out, secs)


def run_benchmarks(args, rep: Reporter, build_dir: str) -> None:
    bench_dir = os.path.join(build_dir, "benchmarks", "primitives")
    for prim, label in FIGURE4_PRIMITIVES:
        num = 2000 if prim == "sort" else 20000
        for kind in ("jit", "background"):
            exe = os.path.join(bench_dir, f"benchmark_{kind}_single")
            code, out, secs = run([*mpirun_command(args), exe, f"--primitive={prim}", f"--num={num}", "--width=64",
                                   "--batch_size=256"], args.timeout * 2)
            ok = code == 0 and "RESULT:" in out and "Test failed" not in out
            rep.record(f"figure 4: {label} [{kind}]", "OK" if ok else "FAIL", out, secs)


# ---------------------------------------------------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "verify"))
    ap.add_argument("--skip-build", action="store_true", help="reuse an existing build in --build-dir")
    ap.add_argument("--quick", action="store_true", help="registered tests only (no extra query configurations, "
                                                         "no benchmarks)")
    ap.add_argument("--sanitizers", action="store_true", help="also run layers 2-9 under ASan/UBSan")
    ap.add_argument("--only", choices=STAGES, action="append", help="run only these sections (repeatable)")
    ap.add_argument("--jobs", type=int, default=4, help="parallel test jobs (default 4)")
    ap.add_argument("--build-jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=180, help="seconds per query run")
    ap.add_argument("--mpirun", default="mpirun")
    ap.add_argument("--mpirun-flags", default=None, help="launcher flags; default --oversubscribe for Open MPI")
    ap.add_argument("--cmake-arg", action="append", default=[], help="extra CMake argument (repeatable)")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)

    selected = set(args.only or STAGES)
    if not args.sanitizers and not args.only:
        selected.discard("sanitizers")
    if args.skip_build:
        selected.discard("build")
    if args.quick:
        selected.discard("benchmarks")
    for tool in ("cmake", "ninja", "ctest", args.mpirun):
        if shutil.which(tool) is None:
            print(f"required tool not found: {tool}")
            return 2

    rep = Reporter(os.path.join(args.build_dir, "verify-logs"), sys.stdout.isatty() and not args.no_color)
    shutil.rmtree(rep.log_dir, ignore_errors=True)
    print(f"Parsec verification (build directory {args.build_dir})")
    start = time.time()
    number = 0

    def section(title: str) -> None:
        nonlocal number
        number += 1
        rep.section(number, title)

    if "build" in selected:
        section("Build")
        ok = configure_and_build(args, rep, args.build_dir, [
            "-DCMAKE_BUILD_TYPE=Release", "-DPARSEC_BUILD_DATABASE=ON", "-DPARSEC_BUILD_BENCHMARKS=ON",
            "-DPARSEC_BUILD_EXPERIMENTS=ON", "-DPARSEC_BUILD_TESTS=ON", "-DPARSEC_WARNINGS_AS_ERRORS=ON"])
        if not ok:
            return rep.summary()
    else:
        number += 1

    layer_keys = [key for key, _, _ in LAYERS]
    if selected & set(layer_keys):
        print("\nRunning the registered tests ...", flush=True)
        results, ctest_out = run_ctest(args, args.build_dir)
        if not results:
            section("Registered tests")
            rep.record("ctest", "FAIL", ctest_out)
        for key, title, prefix in LAYERS:
            if key not in selected:
                number += 1
                continue
            section(title)
            count = report_layer(rep, results, prefix)
            if key == "queries" and not args.quick:
                run_query_configs(args, rep, args.build_dir)
            elif count == 0:
                rep.record("no tests registered", "SKIP")
        # Tests whose names fit no layer would otherwise go unreported.
        prefixes = tuple(prefix for _, _, prefix in LAYERS)
        stray = sorted(n for n in results if not n.startswith(prefixes))
        if stray and not args.only:
            rep.section(0, "Other registered tests")
            for name in stray:
                status, output, secs = results[name]
                rep.record(name, status, output, secs)
    else:
        number += len(LAYERS)

    if "benchmarks" in selected:
        section("Benchmarks (smoke runs)")
        run_benchmarks(args, rep, args.build_dir)
    else:
        number += 1

    if "sanitizers" in selected:
        section("Sanitizers (ASan/UBSan, layers 2-9)")
        san_dir = args.build_dir + "-sanitizers"
        if configure_and_build(args, rep, san_dir, [
                "-DCMAKE_BUILD_TYPE=Debug", "-DPARSEC_ENABLE_SANITIZERS=ON", "-DPARSEC_BUILD_DATABASE=OFF",
                "-DPARSEC_BUILD_BENCHMARKS=OFF", "-DPARSEC_BUILD_EXPERIMENTS=OFF", "-DPARSEC_BUILD_TESTS=ON",
                "-DPARSEC_WARNINGS_AS_ERRORS=ON"]):
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
            results, ctest_out = run_ctest(args, san_dir, env=env)
            if not results:
                rep.record("ctest", "FAIL", ctest_out)
            for _, title, prefix in LAYERS:
                for name in sorted(n for n in results if n.startswith(prefix)):
                    status, output, secs = results[name]
                    rep.record(f"{title.lower()}: {display_name(name, prefix)}", status, output, secs)

    code = rep.summary()
    print(f"total time {time.time() - start:.0f}s")
    return code


if __name__ == "__main__":
    sys.exit(main())
