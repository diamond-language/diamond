# Real application memory soak

This Linux-only harness compares the RC3 tag with the committed `1.0` branch
using standalone Pheint and SkindicateV3 servers. It uses fresh local databases,
the same app/dependency snapshots for both revisions, JIT enabled at threshold
32, and one or four workers. It needs sibling `pheint` and `SkindicateV3`
repositories, GCC with C23 support, Ruby, SQLite, `taskset`, and normal loopback
and `/proc` access.

```sh
bash bench/application_soak/prepare.sh /path/to/fresh-workspace
timeout 15 ruby bench/application_soak/test_sampler.rb
bash bench/application_soak/launch.sh /path/to/fresh-workspace
```

The native comparison builds use `-O3`, `x86-64-v3`, and LTO, with PGO disabled
for consistent compiler settings. Production deployment builds continue to use
the Ubuntu build VM runbook. `revisions.txt`, binary SHA-256 manifests, cut
hashes, machine information and a frozen harness snapshot record provenance.
Run the matrix sequentially, with other builds and tests stopped. Allow about
40 minutes plus builds and seeding. Keep raw databases, logs and binaries in the
workspace; commit aggregate results and the methodology.

Each server gets 30 seconds of warm-up, three 60-second load windows separated
by ten-second quiet intervals, and a final 30-second quiet interval. Eight
clients run on CPUs 8–11; servers run on CPUs 0,2,4,6. The revision order is
reversed between worker counts. `SOAK_*` timing overrides support short harness
smokes; these are separate from the full matrix used for the release decision.

Pheint reuses its committed benchmark's populated GraphQL preflight and client
pool. The blended session exercises nested game/discovery/profile/dashboard
reads, scores, achievements and versioned cloud saves. Challenge-run inserts
are replaced with additional save updates to keep measured application state
bounded. The post-check reads all player identities and every loaded save slot,
then checks a new versioned write, conflict preservation and score improvement
with read-backs. Each report retains actual per-round counts and errors.
Pheint counts the selected load requests; version lookups can issue additional
HTTP calls. Skindicate counts each checked HTTP response, including read-backs.
Compare throughput within each application.

Skindicate creates 128 visible Winamp skins and seeded discussions. Eight
authenticated actors repeatedly validate populated HTML and JSON pages,
settings, favorite/unfavorite read-backs, and edits of their own comments.
Preflight also checks CSRF rejection. Final checks require exact bounded row
counts, SQLite integrity and foreign-key consistency. Both apps use real-disk
SQLite databases during measurement.

The unmodified runtime's tracing does not report standalone worker heaps on
Gremlin's direct process exit. Both source snapshots therefore receive the same
measurement-only `instrument.rb` overlay. It records the first and every 256th
natural major/minor collection, with monotonic time, thread identity, JIT count
and live heap bytes. It does not force collections. These instrumented builds
establish GC/JIT activity during the soak; the overlay is kept outside the
release runtime source.

An independent sampler records process RSS, PSS, private dirty memory, swap,
thread count and database/WAL size every second. Its PID/start-time check avoids
following a reused process ID. The default two-million-KiB RSS watchdog and
free-memory margin protect the workstation; a trip fails the run. The sampler
test exercises normal shutdown, phase changes, timestamps and the kill cap.
Every cell has an overall deadline and confines cleanup to its own process
group. Data and logs remain available after failure.

`summarize.rb` requires all eight cases, sustained-load and quiet samples, real
GC/JIT evidence, no swap or watchdog trips, and semantic success. It reports
late-load and quiet RSS, PSS, retained memory across the last round, and the
late private-memory slope. Candidate growth above eight MiB across the final
round or four MiB/minute over the late loaded samples requires review before
preparing the release. A bounded soak provides evidence over its observed
duration; it does not prove absence of every long-term leak. Throughput is a
secondary observation on the workstation, not a fixed pass threshold.
