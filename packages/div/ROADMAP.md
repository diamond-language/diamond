# packages/drb roadmap

This document is the same kind of running log
[`packages/arel/ROADMAP.md`](../arel/ROADMAP.md) keeps -- future
directions and open decisions specific to this package, with completed
work described in [`README.md`](README.md) rather than duplicated here.

## Deliberately out of scope for v1

Each of these was considered and explicitly deferred, not overlooked:

- **Whitespace-trimming tags** (`<%- -%>`). Real ERB implementations add
  this once generated output's incidental blank lines become a visible
  problem for a real consuming app; no such need has surfaced yet here.
- **A layout/`yield` mechanism.** A compiled template is an ordinary
  function, so one template calling another already covers the common
  "partial" case with no special mechanism (see README's "Partials").
  A layout wrapping arbitrary child content via `yield` is a genuinely
  different shape (the child needs to run *inside* the layout's own
  control flow) and would need real design work, not just wiring.
- **Nested-tag-aware scanning.** `scan` (`lib/drb/compiler.di`) stops at
  the first `%>` after a `<%`, so a tag whose own code contains that
  literal substring (inside a nested string literal, say) terminates
  early. Matches real ERB's own historical simplicity; revisit only if a
  real template needs it.
- **A general filename sanitizer.** `function_name_for` only replaces
  `.` and `-` (the two characters realistically found in a template
  filename: the stripped `.drb` extension leaving `.html`, and
  hyphenated names). Other non-identifier characters in a filename are
  an unhandled, narrow scope cut.

## Open questions

- **Cross-directory basename collisions.** Two input files sharing a
  basename in different directories (`views/users/show.html.drb` and
  `views/posts/show.html.drb`, say) still generate the same function
  name and collide if both are `require`d into one program. A real fix
  needs either a directory-derived prefix or a genuine namespacing
  mechanism -- neither implemented; revisit if this shows up in practice
  rather than solving it preemptively.
- **A `facet`/Makefile build step.** Right now `drbc.di` is invoked by
  hand per file. A `make templates` -style batch-compile convenience (all
  `**/*.html.drb` under a directory) would be a natural small addition
  once a real consuming app's workflow asks for it.
