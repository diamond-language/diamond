# Contributing

Diamond is a personal research language (see [README.md](README.md)). This
file documents how work actually gets done in this repository -- for
future-me as much as anyone else -- not a generic open-source intake process.

## Before making a change

Read [docs/roadmap.md](docs/roadmap.md) first. It states current priorities
and, just as importantly, what's *explicitly deferred* (Ruby compatibility,
a hosted package registry, a JIT without profiling evidence, ...). A change
that fights the roadmap's own stated direction needs a reason, not just an
implementation.

For anything beyond a small fix, skim
[docs/internal/design.md](docs/internal/design.md) and the relevant topic
guide under `docs/` before touching `src/` or `lib/core.di` -- the
architecture section of README.md links the full set.

## Build and test

```sh
make            # debug build (default)
make release    # -O3, no assertions
make sanitize   # ASan/UBSan
make tsan       # ThreadSanitizer
make lsp        # build/diamond-lsp
```

Test targets, from fastest/narrowest to slowest/broadest:

```sh
make test           # debug build + native/case suite -- run this before every commit
make test-lsp        # tests/lsp_test.sh, if lsp/ changed
make test-repl        # tests/repl_test.sh, if src/repl.c changed
make test-<package>   # e.g. make test-rack-package -- if one package changed
make test-all          # debug, release, sanitizers, packages, LSP, REPL, fuzz smoke,
                        # self-host bootstrap -- slow; CI runs this, not every local commit
make test-self-host    # ~1400-case lexer/parser differential corpus -- periodic, not per-push
```

At minimum, `make test` must pass before a commit. If the change touches
`lsp/`, also run `make test-lsp`. See README.md for required system
dependencies and the sanitizer/ptrace note relevant to running in a
container.

## Code style

There is no `.clang-format` (the existing style is dense and specific --
see any file under `src/` or `lsp/`) and no linter gate; match the
surrounding file by eye. In brief: minimal whitespace (`if(x){y;}`, not
`if (x) { y; }`), `nullptr`/`constexpr`/C23 features used freely, comments
that explain *why* (a constraint, a prior incident, a non-obvious
invariant) rather than *what* the code already says. A comment that's
gone stale relative to the code it describes is treated as a real defect,
not cosmetic -- see how often existing comments cite a specific empirical
result, a specific prior bug, or a specific alternative considered and
rejected.

Diamond-language style (`lib/`, `packages/`, `selfhost/`) follows
[docs/internal/di-modernization-audit.md](docs/internal/di-modernization-audit.md)'s
findings: prefer receiver syntax (`values.map(f)`) over the equivalent
free function (`array_map(values, f)`) at any call site being touched
anyway, compound assignment over `x = x + 1`, and `unless`/truthiness
over `if x != nil` only where the nil-vs-false distinction is genuinely
irrelevant.

## Commits

Imperative, present-tense summary line (`Add X`, `Fix Y`, not `Added`/
`Fixed`); a package-scoped change is often prefixed (`packages/rack: add
ContentNegotiation`). Explain *why* in the body when it isn't obvious from
the summary -- `git log` is read far more often than it's written. Keep a
commit to one logical change; split unrelated work into separate commits
even when it landed in the same session.

## Documentation and the changelog

Three different places, three different jobs -- don't blend them:

- **`docs/*.md`** -- current, accurate behavior. Update the relevant guide
  in the same commit as the behavior change it describes.
- **`CHANGELOG.md`** -- a concise, user-visible capability milestone under
  `## Unreleased`, not every implementation step. See its own "Maintenance
  policy" section.
- **`docs/roadmap.md`** -- forward-looking only. When roadmap work lands:
  document the behavior (above), record the changelog milestone (above),
  then remove the completed item from the roadmap itself -- see its own
  "Completion policy" section. A roadmap entry that stays stale after the
  work is done is worse than no entry.

## Scope discipline

This repo's own history is unusually candid about cutting scope
deliberately (see docs/roadmap.md's "Explicitly deferred" section, and
how often a commit message says what it *didn't* do and why). Prefer that
same discipline: a fix doesn't need an adjacent refactor, a new API
doesn't need speculative flexibility nobody has asked for yet, and a
measured claim ("compile is 90-96% of wall time, measured on...") beats
an unmeasured one every time.
