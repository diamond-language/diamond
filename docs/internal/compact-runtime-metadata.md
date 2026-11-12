# Compact runtime metadata

RC3 made type sets compact on disk, but cache loading still expanded each set
into a 354-byte record with eight member slots, then copied that record again.
String constants similarly expanded into 4 KB buffers. Most type sets have one
member; most strings in these tables are short method names.

The `1.0` implementation keeps stable table indices and changes the backing
storage. A string entry contains a character pointer and a length, with an
exact-size, NUL-terminated payload. A type-set entry contains a member pointer,
used count, capacity and inferred flag. It allocates the members it uses.
The literal length limit and eight-member union limit remain the same.

During compilation and ProgramBuilder mutation, stable function-owned arena
blocks hold the payloads. Growing an index table cannot invalidate its payloads.
Scratch type-set builders have block-scoped member arrays and are copied into
function storage before escaping their scope. A mutable scratch copy also copies
its members, preserving the value semantics of the previous inline records.

Immutable function clones put all six index/code tables, used type members and
string characters in one independently owned allocation. This covers cache and
embedded-program loading, prelude/template copying, and thread cloning. Empty
bytecode functions with metadata also retain a freeable allocation base.
Temporary generic-binding graphs explicitly provide their own member storage.
Source metadata and input cache buffers can be freed after copying.

The cache format advances to 22 because the function image now contains metadata
ownership state. That pointer is cleared before serialization and rebuilt by the
reader. Existing caches miss and rebuild. The length-prefixed string and compact
type-member encodings remain unchanged.

## Pheint measurement

Measured on the workstation on 2026-11-11 with optimized release builds: RC3
`fdf6ab48` and the compact-metadata candidate. The fixture uses Pheint
`daf7e8d85798c6aa52a85bf24b818512fa7a2939` and its installed locked cuts:
gremlin 0.5.2, graphql 0.3.3, mjolnir 0.4.0, logger 0.5.1, http 0.2.3,
arel 0.35.3. The application definitions and all requires are retained; its final
`boot()` and `gremlin_serve(...)` calls are replaced with a marker and a read of
`/proc/self/status`. This isolates metadata loading from database state and worker
heaps. It measures memory, not API throughput.

For each binary, `cache warm` prepares a private fixture cache, then three
subprocesses load it with `DIAMOND_TRACE_CACHE=1`. Every run must report a cache
hit and the marker. `/proc/self/status` supplies resident memory after loading;
`/usr/bin/time -f %M` supplies peak RSS. Linux reports these values in KiB.

| Measurement | RC3 | Compact metadata |
| --- | ---: | ---: |
| Cache bytes | 7,276,890 | 7,277,084 |
| Resident memory, three-run median | 213.97 MiB | 58.97 MiB |
| Cache-load peak RSS, three-run median | 219.54 MiB | 64.61 MiB |
| Cold compilation peak RSS, observed range | 432.64–445.26 MiB | 120.94–120.96 MiB |

Resident memory falls about 72%. Remaining memory includes function records,
bytecode/source maps, actual type members, class tables, the VM, and libraries.
Disk size still benefits from zero-run function images and trimmed callable
parameter lists, so it is smaller than the resident program.

The compiled-prelude regression test also clones 5,000 singleton type sets and
1,000 short strings under a 512 KiB combined-payload budget, checks independent
ownership after source mutation and teardown, and covers embedded NUL, empty and
maximum-length strings. Cache tests exercise an eight-member union together with
nested collections and a typed Callable.
