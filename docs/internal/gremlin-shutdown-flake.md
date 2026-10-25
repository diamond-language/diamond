# Gremlin shutdown flake: 2026-10-23

The owner accepted this isolated failure for 0.19.0. No runtime cause has been
established and no assertion or timeout was weakened. The confirmed finding is
a harness bug that hid stderr after a successful connection retry.

## Original evidence

- [Failed job 120332146315](https://github.com/diamond-language/diamond/actions/runs/39928947356/job/120332146315),
  run `39928947356`, attempt 1: `test-all (clang, test-integration, fedora, fedora:latest)`;
  source head `532c0c915011346639ab0cffde09eb5eb7ece4f0`.
- [Diagnostic rerun 120341374653](https://github.com/diamond-language/diamond/actions/runs/39928947356/job/120341374653),
  attempt 2: same source, passed. The later documentation-only head also passed full CI.
- Both used runner 2.338.0, Ubuntu 24.04.5 host image `20261012.334.1`,
  provisioner `20261012.601`, and Fedora image digest
  `sha256:43b29f65a41eb9c35e1cd5323e3bdf3b655c2357a9f4f1ff2f9c2798e5045d80`.
  Clang was `22.1.8-4.fc44`. All 249 Fedora package-version strings extracted
  from the logs matched. These are not a complete installed RPM inventory,
  which the original workflow did not record.
- Failed runner `1000014542` was in `northcentralus`; successful runner
  `1000014547` was in `centralus`. Different runners/regions are a difference,
  not evidence of a cause. The original waited about 125 seconds for a runner.
- Both complete log archives and job metadata were retrieved. Only the rerun
  includes Runner/Worker diagnostic logs. There are no separately uploaded
  artifacts. No CPU, memory-pressure, host-kernel, or per-process resource
  measurements were recorded for the original; rerun diagnostics cannot
  reconstruct that host's state.

All 19 earlier Gremlin checks and all seven interpreted shutdown modes passed.
The compiled single-worker suite then printed `passed idle` and `passed silent`,
followed by Make error 70. The next mode is `slow`, which writes an 8 MiB response
to a client with a 4 KiB receive buffer. No scenario dump, runtime error, or shell
ERR diagnostic appeared. Log receipt times are 09:07:24.0019556Z for `silent`
and 09:07:24.0592191Z for failure; buffering means this is not a reliable 57 ms
execution-time bound. Startup, transfer, and cleanup cannot be distinguished.

## Confirmed diagnostic gap

The retry at `packages/gremlin/test.sh` used:

```bash
if exec 4<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then break; fi
```

On success, `exec` permanently redirects the shell's stderr to `/dev/null`.
Diamond and `report_failure` inherit it. A minimal Bash probe emitting a marker
afterward captured zero stderr bytes; scoping the redirection captured the
expected 16-byte marker. The harness now uses:

```bash
if { exec 4<>"/dev/tcp/127.0.0.1/$port"; } 2>/dev/null; then break; fi
```

This explains the missing error channel, not the exit 70.

The missing stdout dump has a second explanation (found 2026-10-25, #332). The
driver rescued with `rescue error: StandardError`, but `Harness::Child` raises
plain Strings ("the program under test exited while a line was expected"), and
a raised String is not a StandardError. So any Child failure skipped the rescue
and its dump. The drivers now rescue every raised value (#347). The absent dump
is therefore no evidence about where the failure happened.

## Reproduction coverage

Before release preparation, all of these passed:

| Probe | Repetitions |
|---|---:|
| GCC compiled single-worker shutdown suite, affinity 0–1 | 25 |
| Host Clang 23.1.3 compiled shutdown suite | 40 |
| Host Clang full Gremlin harness, separate trace FD, affinity 0–1 | 8 |
| Pre-migration `dfe3be29` full harness with host Clang | 12 |
| Exact Fedora image and Clang 22.1.8 full harness | 20 |

The cached Fedora image's repository digest matches the failed CI image.
All 5,552 tracked file contents in the Fedora reproduction tree were checked
against Git blob hashes from `532c0c91`; none differed. Its local Fedora host
kernel differs from the unrecorded GitHub kernel.
The additional exact-image probe passed 15 quiet and 15 CPU-contended complete
single-worker shutdown suites (all seven modes), followed by one successful
`strace -ff -ttt` run covering process, signal, network, write, close, and dup2
calls. The container used `--cpus=2`; contention was one background
`sha256sum /dev/zero` process. CPU quotas are experiments, not recovered facts
about the failed runner. The host kernel was `7.2.11-300.fc45.x86_64`.
The full installed RPM inventory is retained as `probe/rpm.txt` in the session
evidence directory, SHA-256
`572a1930b3654c384852c2aca7e08988769d6a5f85f294887e45014c683bbff7`.

Do not export `SHELLOPTS=xtrace` into Make: Bash POSIX-mode inheritance makes a
failed `/dev/tcp` redirection fatal, creating an artificial startup failure.
Use `BASH_XTRACEFD` on a dedicated descriptor and invoke `bash -x` directly.
The AOT compiler option is `--cc=clang`, with `=`. Missing local cut fixtures or
invalid probe arguments are setup errors, not reproductions of this flake.

## Evidence and next step

Raw evidence remains in `/tmp/diamond-d6` and `/tmp/diamond-release-019` for
this session; scratch storage is not permanent. Retrieve linked CI attempts
again before retention expires if needed. Job-log SHA-256 fingerprints:

```text
a481408191818bc9d0a40dc3a3674edd4da6ab77ba6bd954b61d43104ced2510  original
8e764ea477e1091287233ea2c4a97d244ca222d6b1cdf4a76f13eb37e17c6c04  rerun
```

On recurrence, retain stderr, the failing mode and child output, runtime/AOT
binaries, installed RPM inventory, kernel/resource facts, and process/socket
syscall traces. Distinguish an assertion, child failure, port-allocation/spawn
failure, and VM error before selecting a fix. More blind repetitions alone
cannot identify the error hidden in the original run.
