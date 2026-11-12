# RC4 standalone application soak

Measured on 2026-11-11 on the development laptop: AMD Ryzen 5 Pro 7535U
(6 cores, 12 logical CPUs), approximately 15 GiB RAM, Fedora Linux x86-64.
The owner reduced other activity to two terminals and their VPN. Builds and
other test suites were stopped throughout the sequential timed matrix.

## Result

All eight cases passed semantic checks, post-load write read-backs, SQLite
integrity and foreign-key checks. No measured server swapped or reached the
RSS watchdog. Actual serving garbage collections and JIT compilation were
observed in every worker configuration.

| Application | Workers | RC3 loaded RSS | Candidate loaded RSS | Reduction | Candidate final quiet RSS | Final-round retained RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Pheint | 1 | 221.47 MiB | 73.92 MiB | 66.6% | 74.80 MiB | −1.08 MiB |
| Pheint | 4 | 650.72 MiB | 244.72 MiB | 62.4% | 246.82 MiB | −1.48 MiB |
| Skindicate | 1 | 100.36 MiB | 49.95 MiB | 50.2% | 49.78 MiB | +0.004 MiB |
| Skindicate | 4 | 356.96 MiB | 152.18 MiB | 57.4% | 151.33 MiB | +0.031 MiB |

Loaded RSS is the median of the latter half of all load samples. Retained RSS
compares the median quiet interval before the last load with the final quiet
interval. Candidate late private-dirty slopes ranged from −0.03 to +0.36
MiB/minute, below the 4 MiB/minute review threshold; retained RSS was below the
8 MiB review threshold. This is evidence of settling over the observed windows,
not a guarantee against an arbitrarily long-lived leak.

Pheint completed 56,231 selected load requests across the four cases; Skindicate
completed 303,067 checked HTTP responses. Pheint's auxiliary version lookups
add HTTP calls beyond its selected-request count. Rates were approximately
65 versus 66 selected requests/s at one worker and 90 versus 91 at four;
Skindicate measured 360 versus 362 responses/s and 479 versus 481. These small
differences are secondary observations on a workstation, not speedup claims.

## Sources and controls

- RC3 runtime: `fdf6ab487f03fde5ece43290714a8f48b0922a16`.
- Candidate runtime: `bb5a04135ff7d92293c30c7d837e86d5b1107f0a` (`1.0`, compact metadata fix).
- Pheint app: `015bd5893d855c40609dbf9d557fe28ed44c11f8`.
- Pheint benchmark source: `daf7e8d85798c6aa52a85bf24b818512fa7a2939` (benchmark directory only).
- Skindicate app: `0c611a4e567091bc2fdf6130e0e3f67d8d898ec0`.

Both runtimes used identical app and dependency snapshots, `-O3`,
`-march=x86-64-v3`, LTO and no PGO. These were native Fedora comparison builds,
separate from the Ubuntu production build procedure. JIT was enabled with
threshold 32. Eight clients used CPUs 8–11; servers used CPUs 0,2,4,6.
Each cell had 30 seconds of warm-up, three 60-second load windows separated
by ten-second quiet intervals, and a final 30-second quiet interval. Pheint
also performed a one-second serial probe each round. All cases ran sequentially,
reversing revision order between worker counts.

The same measurement-only overlay in both archived runtime snapshots reported
the first and every 256th natural collection, thread identity, JIT count and
live bytes. It did not force GC and is absent from the released runtime source.
An independent sampler recorded `/proc` memory every second. A two-million-KiB
RSS cap, process-identity checks and process-group deadlines bounded failures.
System swap already contained unrelated pages at the start; the measured
server processes recorded zero swap throughout.

Pheint exercised populated nested GraphQL reads, scores, achievements and
versioned save updates. Its 40 preflight and seven post-load assertions passed
in every case, including all 64 player identities and all 64 loaded save slots.
Post-load writes checked version conflicts, preserved bytes and best-score
semantics. Save updates replaced unbounded challenge-run insertion during load.
Skindicate exercised populated pages, JSON, profiles/settings, favorite changes
and comment edits with read-backs for eight authenticated actors. Its final
counts exactly matched nine users, 128 skins, 136 comments, eight sessions,
zero favorites and zero downloads. Pheint's intentional final-check insertions
account for its small row-count differences; the aggregate records include both
warm and final counts.

## Reproduction and evidence

The [harness README](../../bench/application_soak/README.md) explains commands,
requirements and failure gates. [Aggregate JSON](../../bench/application_soak/results-2026-11-11.json)
preserves all eight cases, PSS, peaks, growth slopes, counts and GC/JIT evidence.
The companion [provenance manifest](../../bench/application_soak/provenance-2026-11-11.txt)
records source, executable, dependency and frozen-harness hashes. Raw logs,
SQLite databases and executables remain in the local `.soak-rc4` workspace;
they are excluded from the repository.

After the timed matrix, both candidate standalone binaries also passed their
full application suites with `DIAMOND_JIT=0` and `DIAMOND_JIT=1`. Test adapters
launched the compiled executable for `app.di`/`app.d6` and the matching candidate
interpreter for client and CLI fixtures. Skindicate covered HTTP/WebSocket,
uploads, chat, transactions, contention, backups and deployment guards; its
legacy V2 migration check was skipped because no V2 database snapshot was
available. These suites ran separately from the timed measurements.
