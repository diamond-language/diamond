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
- **A general filename sanitizer.** `function_name_for` only replaces
  `.` and `-` (the two characters realistically found in a template
  filename: the stripped `.div` extension leaving `.html`, and
  hyphenated names). Other non-identifier characters in a filename are
  an unhandled, narrow scope cut.

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
  exist now, see docs/design.md's "Splat/variadic parameters" and
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

## Open questions

- **Cross-directory basename collisions.** Two input files sharing a
  basename in different directories (`views/users/show.html.div` and
  `views/posts/show.html.div`, say) still generate the same function
  name and collide if both are `require`d into one program. A real fix
  needs either a directory-derived prefix or a genuine namespacing
  mechanism -- neither implemented; revisit if this shows up in practice
  rather than solving it preemptively.
- **A `facet`/Makefile build step.** Right now `divc.di` is invoked by
  hand per file. A `make templates` -style batch-compile convenience (all
  `**/*.html.div` under a directory) would be a natural small addition
  once a real consuming app's workflow asks for it.
