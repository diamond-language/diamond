#!/usr/bin/env python3
"""Build and test Diamond on this machine and write a report to send back.

Meant to be run by someone who isn't a Diamond developer:

    python3 tools/platform_report/platform_report.py

It works in a throwaway copy of this checkout, asks before installing
anything, keeps going when a step fails, and writes one text file (on the
Desktop when there is one) that holds everything a maintainer needs.
Standard library only; tested against macOS's bundled Python 3.
"""
import datetime
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]
IS_MAC = platform.system() == "Darwin"
BREW_PACKAGES = ["coreutils", "gnu-sed", "bash", "ripgrep", "openssl@3", "sqlite",
                 "postgresql@16", "mariadb-connector-c"]
# (benchmark, in-process repeats), a representative slice of bench/*.di.
BENCHMARKS = [("fibonacci", 15), ("int_arithmetic", 15), ("dispatch_monomorphic", 15),
              ("dispatch_polymorphic", 12), ("closures", 25), ("array_ops", 30),
              ("hash_ops", 40), ("string_ops", 40), ("struct_field_access", 15),
              ("regexp_match", 15), ("tensor_matmul", 20), ("exception_handling", 12),
              ("fiber_switch", 10), ("channel_message_passing", 15), ("thread_pool", 15)]
# (make target, rough duration shown so a quiet step doesn't look stuck)
TEST_TARGETS = [("test", "the main suite: 5-15 minutes"), ("test-api", "about a minute"),
                ("test-fibers", "about a minute"), ("test-facet", "1-3 minutes"),
                ("test-aot-cache", "about a minute"), ("test-aot-kit", "1-2 minutes"),
                ("test-lsp", "about a minute"), ("test-repl", "under a minute")]
report_lines = []


def say(text=""):
    print(text, flush=True)


def record(text=""):
    report_lines.append(text)


def section(title):
    say()
    say(f"== {title} ==")
    record()
    record(f"## {title}")


def run(args, cwd=None, env=None, timeout=None):
    """Runs a command and returns (exit status, combined output, seconds)."""
    start = time.perf_counter()
    try:
        result = subprocess.run(args, cwd=cwd, env=env, timeout=timeout, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        status, output = result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        status, output = -1, (error.stdout or "") + "\n[timed out]"
    except OSError as error:
        status, output = -1, str(error)
    return status, output, time.perf_counter() - start


def capture(args):
    status, output, _ = run(args, timeout=30)
    return output.strip() if status == 0 else ""


def tail(text, lines=40):
    return "\n".join(text.rstrip().splitlines()[-lines:])


def ask(question):
    try:
        return input(f"{question} [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def machine_info():
    section("Machine")
    facts = [("Date", datetime.datetime.now().isoformat(timespec="seconds")),
             ("System", " ".join(platform.uname()[:3])),
             ("Architecture", platform.machine())]
    if IS_MAC:
        facts += [("macOS", capture(["sw_vers", "-productVersion"])),
                  ("Chip", capture(["sysctl", "-n", "machdep.cpu.brand_string"])),
                  ("CPU cores", capture(["sysctl", "-n", "hw.ncpu"])),
                  ("Performance cores", capture(["sysctl", "-n", "hw.perflevel0.physicalcpu"])),
                  ("Efficiency cores", capture(["sysctl", "-n", "hw.perflevel1.physicalcpu"])),
                  ("Memory bytes", capture(["sysctl", "-n", "hw.memsize"]))]
    else:
        facts += [("CPU cores", str(os.cpu_count()))]
    facts += [("C compiler", capture(["cc", "--version"]).splitlines()[0] if capture(["cc", "--version"]) else "missing"),
              ("Diamond commit", capture(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"]))]
    for name, value in facts:
        say(f"  {name}: {value or 'unknown'}")
        record(f"{name}: {value or 'unknown'}")


def prerequisites():
    """Checks tools a person must install themselves. Returns False to stop."""
    section("Prerequisites")
    if IS_MAC and subprocess.run(["xcode-select", "-p"], capture_output=True).returncode != 0:
        say("Apple's command line developer tools are missing. Install them with:")
        say("    xcode-select --install")
        say("then run this script again.")
        record("Xcode command line tools: missing (stopped)")
        return False
    if IS_MAC and shutil.which("brew") is None:
        say("Homebrew is missing. Install it by following https://brew.sh, then run this")
        say("script again. It installs the libraries Diamond builds against.")
        record("Homebrew: missing (stopped)")
        return False
    if not IS_MAC:
        say("Not macOS: skipping Homebrew; the build uses this system's own libraries.")
        record("Homebrew: not used (not macOS)")
        return True
    installed = set(capture(["brew", "list", "--formula", "-1"]).split())
    missing = [package for package in BREW_PACKAGES if package not in installed]
    if missing:
        say("Diamond needs these Homebrew packages:")
        say("    " + " ".join(missing))
        if not ask("Install them now with Homebrew?"):
            say("OK, stopping. Run `brew install " + " ".join(missing) + "` and try again.")
            record("Homebrew packages: declined install (stopped)")
            return False
        status, output, seconds = run(["brew", "install", *missing])
        record(f"brew install {' '.join(missing)}: exit {status} in {seconds:.0f}s")
        if status != 0:
            say("Homebrew couldn't install everything; the report records why.")
            record(tail(output))
            return False
    record("Homebrew packages: present")
    say("  everything needed is installed")
    return True


def build_environment():
    """PATH and make arguments matching the macOS CI job."""
    env = dict(os.environ, DIAMOND_CASE_RUNNER="subprocess")
    make_args = []
    if IS_MAC:
        prefix = {package: capture(["brew", "--prefix", package]) for package in BREW_PACKAGES}
        env["PATH"] = os.pathsep.join([f"{prefix['coreutils']}/libexec/gnubin",
                                       f"{prefix['gnu-sed']}/libexec/gnubin",
                                       f"{prefix['bash']}/bin", env.get("PATH", "")])
        includes = " ".join(f"-I{prefix[p]}/include" for p in
                            ["openssl@3", "sqlite", "postgresql@16", "mariadb-connector-c"])
        includes += f" -I{prefix['mariadb-connector-c']}/include/mariadb"
        libs = " ".join(f"-L{prefix[p]}/lib" for p in
                        ["openssl@3", "sqlite", "postgresql@16", "mariadb-connector-c"])
        libs += f" -L{prefix['mariadb-connector-c']}/lib/mariadb"
        make_args = ["CC=clang", "LDLIBS_CRYPT=", f"CPPFLAGS_EXTRA={includes}",
                     f"LDFLAGS_EXTRA={libs}"]
    return env, make_args


def exclude_known_gaps(work):
    """The same exclusions as the macOS CI job: BCrypt (no crypt_gensalt on
    macOS) and cases that exercise the glibc-only x86-64 JIT."""
    cases = work / "tests/cases"
    removed = 0
    for name in (["active_record_secure_password", "bcrypt"] if IS_MAC else []):
        for suffix in [".di", ".expected"]:
            path = cases / (name + suffix)
            if path.exists():
                path.unlink()
                removed += 1
    if IS_MAC or platform.machine() not in ("x86_64", "AMD64"):
        for env_file in cases.glob("*.env"):
            if "DIAMOND_JIT=" not in env_file.read_text():
                continue
            for path in cases.glob(env_file.stem + ".*"):
                path.unlink()
                removed += 1
    return removed


def build_and_test(work, env, make_args, jobs):
    section("Build and tests")
    results = []
    say("  Each step is quiet while it runs; that's normal.")
    for target, duration in TEST_TARGETS:
        say(f"  running make {target} ({duration}) ...")
        status, output, seconds = run(["make", f"-j{jobs}", *make_args, target], cwd=work,
                                      env=env, timeout=3600)
        verdict = "passed" if status == 0 else "FAILED"
        last = output.strip().splitlines()[-1] if output.strip() else ""
        say(f"    {verdict} in {seconds:.0f}s  {last[:80]}")
        record(f"make {target}: {verdict} in {seconds:.0f}s -- {last}")
        if status != 0:
            record("```")
            record(tail(output))
            record("```")
        results.append(status == 0)
    return all(results)


def examples(work, env):
    section("Examples")
    status, output, seconds = run(["bash", "examples/logstat/smoke_test.sh"], cwd=work,
                                  env=env, timeout=900)
    verdict = "passed" if status == 0 else "FAILED"
    say(f"  logstat (diamond build): {verdict} in {seconds:.0f}s")
    record(f"logstat smoke test: {verdict} in {seconds:.0f}s")
    if status != 0:
        record(tail(output))
    chat = work / "examples/chat"
    status, output, seconds = run([str(work / "build/facet"), "install"], cwd=chat, env=env,
                                  timeout=300)
    if status == 0:
        status, output, more = run([sys.executable, "smoke_test.py"], cwd=chat, env=env,
                                   timeout=300)
        seconds += more
    verdict = "passed" if status == 0 else "FAILED"
    say(f"  chat (registry install + WebSockets): {verdict} in {seconds:.0f}s")
    record(f"chat smoke test (facet install from cuts.dilang.tech): {verdict} in {seconds:.0f}s")
    if status != 0:
        record(tail(output))


def benchmarks(work, env, make_args, jobs):
    section("Benchmarks (release build, interpreter)")
    say("  building an optimized copy (a few minutes) ...")
    status, output, seconds = run(["make", f"-j{jobs}", *make_args, "release"], cwd=work,
                                  env=env, timeout=3600)
    if status != 0:
        say("  release build FAILED; skipping benchmarks")
        record("release build FAILED")
        record(tail(output))
        return
    record(f"release build: {seconds:.0f}s")
    diamond = str(work / "build/diamond")
    bench_env = dict(env, DIAMOND_NO_CACHE="1")
    record("benchmark                   repeats  seconds/run")
    for name, repeats in BENCHMARKS:
        path = work / "bench" / f"{name}.di"
        if not path.exists():
            continue
        status, _, seconds = run([diamond, str(path)], env=dict(bench_env, DIAMOND_REPEAT=str(repeats)),
                                 timeout=600)
        line = f"{name:<27} {repeats:>7}  " + (f"{seconds / repeats:10.4f}" if status == 0 else "    FAILED")
        say("  " + line)
        record(line)

    say()
    say("  thread scaling (same work per thread; flat time = perfect scaling)")
    say("  (up to a minute per line)")
    record()
    record("thread scaling: each thread does the same work; ideal is flat time")
    record("threads   seconds   work/second relative to 1 thread")
    cores = os.cpu_count() or 8
    counts = sorted({1, 2, 4, 8, 16, 32, cores} & set(range(1, min(cores, 32) + 1)))
    script = str(Path(__file__).with_name("thread_scaling.di"))
    baseline = None
    for count in counts:
        status, _, seconds = run([diamond, script, str(count), "20000000"], env=bench_env,
                                 timeout=1200)
        if status != 0:
            line = f"{count:>7}   FAILED"
        else:
            rate = count / seconds
            baseline = baseline or rate
            line = f"{count:>7}   {seconds:7.2f}   {rate / baseline:6.2f}x"
        say("    " + line)
        record(line)


def main():
    say("Diamond platform report")
    say("This builds Diamond, runs its tests and some benchmarks, and writes a")
    say("report file to send back. It takes roughly 20-40 minutes. Your checkout is")
    say("not modified; the work happens in a temporary copy.")
    record("# Diamond platform report")
    machine_info()
    if not prerequisites():
        write_report()
        return 1
    env, make_args = build_environment()
    jobs = os.cpu_count() or 4
    with tempfile.TemporaryDirectory(prefix="diamond-report-") as temporary:
        work = Path(temporary) / "diamond"
        # --no-hardlinks: the temporary directory may be on another filesystem.
        status, output, _ = run(["git", "clone", "--quiet", "--no-hardlinks", str(REPO), str(work)])
        if status != 0:
            say("Could not make a temporary copy of the checkout:")
            say(tail(output, 5))
            record("could not copy the checkout:")
            record(tail(output))
            write_report()
            return 1
        record(f"excluded known-gap test files: {exclude_known_gaps(work)}")
        tests_passed = build_and_test(work, env, make_args, jobs)
        examples(work, env)
        benchmarks(work, env, make_args, jobs)
    section("Summary")
    summary = "all test targets passed" if tests_passed else "some test targets FAILED (details above)"
    say("  " + summary)
    record(summary)
    write_report()
    return 0


def write_report():
    desktop = Path.home() / "Desktop"
    folder = desktop if desktop.is_dir() else Path.home()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    path = folder / f"diamond-report-{stamp}.txt"
    path.write_text("\n".join(report_lines) + "\n")
    say()
    say(f"Report written to {path}")
    say("Please send that file back. Thanks!")


if __name__ == "__main__":
    sys.exit(main())
