# Investigation: Knuth-style literate programming for Diamond

Status: investigation only (branch `lp`). Nothing here changes the build. Written 2026-10-20 against `main` at the time the branch was cut.

## What "Knuth-style" would mean here

A WEB: each source file is one document in which prose and code interleave in the order that best *explains* the program, code is written as named chunks (`<<read the next token>>=`) that may refer to other chunks, and two tools derive the artefacts: **tangle** (chunks reassembled into compilable source in compiler order) and **weave** (the same document typeset with a table of contents, chunk cross-references and an index of identifiers). The point is exposition order and a single place where explanation and code cannot drift apart. It is not just "more comments".

## What would be converted

| Area | Lines | Notes |
|---|---|---|
| `src/*.c`, `src/*.h` | 66,700 | `compiler.c` 20.3k, `vm.c` 14.6k, `vm_run_chunk.c` 6.2k, `jit.c` 2.6k |
| `lsp/`, `dap/` | 7,600 + | C |
| `lib/*.di` (the prelude) | 2,600 | compiled into the binary at build time |
| `packages/*/lib` | 15,900 | Diamond; **published as source archives** |
| `tests/cases` | 40,900 | one small program per case; not a candidate |
| `docs/*.md`, `docs/internal/*.md` | 16,800 | already prose; `design.md` (121 KB) and `jit-design.md` (140 KB) are essentially the narrative a WEB would contain, kept apart from the code they describe |

Realistic scope is the C core and the packages (about 90,000 lines). Tests stay as they are.

## What is already in our favour

- The documentation is already Markdown and already deep. Much of `docs/internal/*.md` would become the prose of the corresponding chunks rather than new writing.
- gcc and clang honour `#line N "file"`, so tangled C can report diagnostics, debugger positions and sanitizer traces against the literate source with no extra tooling.
- The loader already prepends a `#line 1` reset (`src/loader.c`, `src/run_source.c`, `src/repl.c`) and the lexer understands it, so a line-mapping directive is not a foreign idea in the Diamond front end.
- The project's convention is that the test dependency set is the built `diamond` binary (`tests/harness.di`). A tangler written in Diamond would fit.

## Obstacles, in order of how much they matter

1. **Bootstrap.** `build/diamond` needs the C sources tangled before it exists, and the prelude (`lib/*.di`) is compiled into the binary during the build. A tangler written in Diamond cannot produce the sources that build Diamond. Options: (a) commit the tangled output and have CI fail when it is stale (the `facet.lock`/inventory pattern this repo already uses); (b) write the tangler in portable shell or awk or a small C program that the Makefile builds first; (c) both: a tiny C tangler for the bootstrap, committed generated files for everyone else. (b)/(c) keep `make` self-contained; (a) alone keeps the build as it is today but makes every change two edits (document, then regenerate) and pollutes diffs.
2. **Diamond has no general line directive.** The lexer's `#line` handling is an internal reset to 1. Tangled `.di` files would report tangled-file positions in compile errors, backtraces, the language server, and the debugger. Needed: `#line N "file"` (or an equivalent source map) in the lexer, the bytecode line tables and the LSP/DAP position code. This is a compiler change in its own right, and it interacts with the position-pinned tests (`tests/run.sh`, `tests/parser_diff.sh`, `tests/multifile`, `tests/parser_*`).
3. **Cuts are distributed as source.** A published cut is an archive of `.di` files that users read, copy and debug, and the registry pins archives by SHA-256 (`docs/registry-launch-inventory.json`). A literate package would publish the *tangled* `.di` (and perhaps the woven document alongside it), `facet` and the registry seed tooling would have to know to tangle first, and tangling must be byte-for-byte reproducible or every inventory entry churns.
4. **The editor experience.** The language server, formatter and `.di` syntax highlighting work on `.di` files. Authors would edit `.md`/`.web` files with nested languages; the LSP would see only generated files unless it learns to map positions back (obstacle 2 again).
5. **Review and agent workflow.** Pull-request diffs of a document with reordered chunks are harder to review than diffs of a source file, and `git blame` follows the document order, not the program order. The branch, PR and merge-on-green procedure in `AGENTS.md` assumes edits to source files that CI builds directly.
6. **Authoring cost is real and mostly not mechanical.** Splitting a file into chunks can be automated; the exposition (choosing an order, writing the explanation, naming chunks) cannot. 67,000 lines of C, with a 20,000-line `compiler.c` in a single translation unit, is a multi-month writing project for one person.
7. **Third-party code.** `vendor/onigmo` and `reginold/` stay as they are.

## Format options

| Option | Pros | Cons |
|---|---|---|
| **noweb** | mature, language-neutral, `weave` to HTML/LaTeX, identifier index with `noindex` | not installed here; Perl/awk/icon toolchain; another dependency on every dev and CI machine |
| **CWEB** (Knuth/Levy) | the original for C; strong cross-referencing | C only (not `.di`); TeX output; `#line` support but an unfamiliar syntax |
| **Org-mode babel** | rich, `:noweb` references, tangle from Emacs | ties authors to Emacs |
| **Literate Markdown, own tooling** (fenced blocks tagged `chunk=name`, `<<name>>` references) | matches the existing docs, renders on GitHub unchanged, tiny tangler (a few hundred lines), works for C and Diamond, can emit `#line` | we write and maintain the tools, including weave cross-references and the identifier index |

Recommendation if the project goes ahead: **literate Markdown with our own small tangler and weaver**, because the docs are already Markdown and a plain-text format keeps `rg`, diffs and GitHub rendering useful. The tangler itself should be about 300 lines of portable C or POSIX shell so that it can run before `diamond` exists.

## Proposed pilot (small, reversible, measurable)

Do not convert the codebase. Convert **one self-contained module** and measure:

- Candidate: `lsp/position.h` (UTF-16/byte column conversion, about 100 lines, has `tests/position_test.c`), or a package of 300–600 lines such as `packages/redis` or `packages/multipart`.
- Deliverables: the tangler (C or shell) with a test for chunk reassembly, `#line` emission and an "output is stale" check; the module as a `.md` WEB; a Makefile rule that tangles it and a CI check that the committed output matches; a woven HTML page.
- Success criteria: the build and `make test` are unchanged; debugger/sanitizer/compiler positions point at the document; the tangled output is byte-identical across machines; and a reviewer can judge whether the document explains the module better than the source plus `docs/` did.
- Decide after the pilot whether obstacle 2 (a `#line N "file"` directive in the Diamond front end) is worth building before touching any package.

## What I would not do

- Convert `compiler.c` or `vm.c` first. They are the largest payoff and the largest risk; they belong after the tools and conventions are proven on small modules.
- Make tangling a build-time requirement for published cuts before the registry seed and inventory have a reproducible tangle.
- Convert `tests/cases` or the vendored code.

## Open questions for the owner

1. Is the goal reading and teaching (a woven book of the implementation), or maintainability of the source itself? The first can be reached with weave alone over lightly annotated sources; the second needs tangle and the bootstrap work above.
2. Should published cuts carry the document, the tangled source, or both?
3. Are you willing to add `#line N "file"` to the Diamond front end, which touches the compiler, bytecode line tables, LSP and DAP?
