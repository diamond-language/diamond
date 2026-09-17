# packages/div roadmap

This document is the same kind of running log
[`packages/arel/ROADMAP.md`](../arel/ROADMAP.md) keeps -- future
directions and open decisions specific to this package, with completed
work described in [`README.md`](README.md) rather than duplicated here.

## Deliberately out of scope for v1

Each of these was considered and explicitly deferred, not overlooked:

- **Whitespace-trimming tags** (`<%- -%>`). Real ERB implementations add
  this once generated output's incidental blank lines become a visible
  problem for a real consuming app; no such need has surfaced yet here.
- **Nested-tag-aware scanning.** `scan` (`lib/div/compiler.di`) stops at
  the first `%>` after a `<%`, so a tag whose own code contains that
  literal substring (inside a nested string literal, say) terminates
  early. Matches real ERB's own historical simplicity; revisit only if a
  real template needs it.

## Resolved

- **A layout/`yield` mechanism -- resolved, needed no new code.** Was an
  open question in this section, described as "a genuinely different
  shape" from partials that "would need real design work." Tracing
  through what real ERB/Rails `yield` actually does -- substitute
  already-rendered child output into a wrapping template -- showed
  otherwise: Diamond's own `yield` is a hard reserved keyword (already
  meaning "suspend the current Fiber," `DIAMOND_TOKEN_YIELD` in
  `src/lexer.c`), so it was never reusable as a placeholder name in the
  first place, and (true when this was written; both `def foo(*rest)`
  variadic parameter *definitions* and `foo(*array)` call-site *spread*
  exist now, see docs/internal/design.md's "Splat/variadic parameters" and
  "Call-site spread" -- but spread's own first version only calls a
  statically-named top-level function, not an arbitrary `Callable`
  *value*, which is exactly what a generic "call whichever layout
  function was passed in" helper would need) Diamond had no way to
  forward an arbitrary caller-supplied argument list into a dynamically-
  chosen call, ruling out a generic helper that forwards an arbitrary
  layout parameter list. What's left is exactly a layout being an ordinary
  template with an ordinary declared local holding the already-rendered
  child output -- no new compiler/translator mechanism, just a naming
  convention. See README's "Layouts" section for the worked example.
  Deliberately left as convention rather than adding opt-in sugar for
  its own sake, matching this repo's own "no macro until a concrete need
  surfaces" pattern (see `packages/arel/ROADMAP.md`'s "additional
  operators" decision).

- **Compiled output moved under `.cache/`.** `divc.di` used to default to
  writing `index.html.di` right next to `index.html.div`; it now writes
  under a `.cache/` subdirectory next to the source instead
  (`views/index.html.div` -> `views/.cache/index.html.di`), creating
  `.cache/` itself (`Process.run(["mkdir","-p",...])`) if it doesn't
  already exist -- self-sufficient regardless of what invokes it, not
  dependent on a caller script remembering to `mkdir -p` first. Motivated
  directly: a generated `.di` file sitting next to hand-written source is
  easy to mistake for something hand-written, and scattered generated
  files are harder to `.gitignore`/clean than one directory. An explicit
  second CLI argument still bypasses this entirely, unchanged.
  `examples/library/compile_views.sh` was also changed to wipe
  `views/.cache/` and rebuild every view fresh on each run, rather than
  only whatever changed -- the translator is fast enough that this costs
  nothing noticeable, and it's the simplest way to guarantee there's
  never a stale compiled file (from a renamed/deleted source, say)
  silently still in use, without needing any mtime/staleness-tracking
  logic at all.

- **A reusable recursive batch build.** Once `examples/project_board`
  became the second Div consumer, its copy of `examples/library`'s flat
  compile loop met this roadmap's stated threshold for generalization.
  `bin/divc_all.sh` now cleanly rebuilds every `*.html.div` below a supplied
  template directory, recursively and with NUL-safe path handling. Both apps'
  `compile_views.sh` files are thin delegates to it. This remains independent
  of `facet`/Make because neither build system needs a new extension point to
  provide the actual shared behavior.

- **Cross-directory basename collisions.** `skindicate.dia` hit exactly
  the "if this shows up in practice" case this used to wait for,
  reorganizing its views into one subdirectory per controller.
  `function_name_for_relative` (`lib/div/compiler.di`) qualifies the
  generated name by every directory segment between a template-tree
  root and the file itself -- `views/users/show.html.div` ->
  `users_show_html`, `views/skins/show.html.div` -> `skins_show_html` --
  and `bin/divc_all.sh` now passes each file's own root-relative path
  through for exactly this, so the directory-derived prefix option this
  section used to name is what got built. A file directly under the
  root is unaffected (same name either function gives it), and
  `bin/divc.di` invoked directly on one file with no known root falls
  back to the old basename-only name -- see README's own naming
  discussion for the full picture.

- **A general filename sanitizer.** `sanitize_identifier` used to only
  replace `.` and `-`, an intentionally narrow scope cut for plain
  filenames. Directory names are a real source of other non-identifier
  characters (a space, confirmed by test.sh's own pre-existing "nested
  folder" fixture) now that they feed into a generated name too, so this
  widened to replace any byte outside `[A-Za-z0-9_]`, not an enumerated
  list of two characters.

## Open questions

Nothing open right now.
