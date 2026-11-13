# D6 1.0.0-rc4

The fourth release candidate for D6 1.0 keeps compact program metadata compact
in memory, extending rc3's smaller bytecode caches and executables.

## Smaller runtime metadata

String constants allocate their actual character count, and type sets allocate
only their used members. Loading a compact program no longer expands each
string into a mostly empty 4 KB buffer or each type set into eight member slots.
Compiler, template and worker copies retain independent ownership of mutable
metadata. Bytecode cache format 22 rebuilds older caches automatically.

## Real application verification

Standalone Pheint and Skindicate servers were compared against rc3 with the JIT
enabled, at one and four workers. Each case used warm-up, three one-minute load
windows and quiet intervals, with real SQLite databases and populated APIs.

| Application | Workers | RC3 loaded RSS | RC4 candidate loaded RSS |
| --- | ---: | ---: | ---: |
| Pheint | 1 | 221 MiB | 74 MiB |
| Pheint | 4 | 651 MiB | 245 MiB |
| Skindicate | 1 | 100 MiB | 50 MiB |
| Skindicate | 4 | 357 MiB | 152 MiB |

All eight runs passed response, write read-back and database integrity checks.
Candidate memory stayed within the soak's growth limits after natural garbage
collections. This bounded workstation measurement does not establish absence
of every long-term leak; throughput differences were small and remain subject
to laptop scheduling and power conditions. The [full report](internal/rc4-application-soak.md)
records the source revisions, instrumentation, aggregate data and limits.

No bundled cuts changed. See the [changelog](../CHANGELOG.md) for the complete
changes. Release preparation does not update deployed services; the
[production record](../applications/registry/deploy/PRODUCTION.md) records actual
deployments and backups.
