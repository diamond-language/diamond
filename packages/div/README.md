# packages/div

An ERB-style view template compiler for
[Diamond](https://gitlab.com/dmn9180/diamond): `.html.div` template
source translates to an ordinary `.di` source file, which you `require`
and call like any other compiled function. Integrates with
[`packages/rack`](../rack/README.md) via one small response adapter
(see below); `packages/rack` itself is untouched, staying
dependency-free per its own design.

## Why offline translation, not runtime `eval`

Diamond has no general `eval(source) -> value` (see
[`docs/roadmap.md`](../../docs/roadmap.md)'s "Explicitly deferred").
`div` doesn't need one: `packages/div/bin/divc.di` translates a
`.html.div` file into a real `.di` source file once, ahead of time --
the output is an ordinary, `require`-able, cacheable compiled artifact,
the same as every other Diamond source file, with zero runtime
dependency on this package.

## Install

Same story as `packages/http`/`packages/rack` -- copy this directory
into another project as `cuts/div/`, or give it its own git remote and
depend on it via `facet` (see
[`docs/packages.md`](../../docs/packages.md)).

## Compiling a template

```
diamond packages/div/bin/divc.di views/index.html.div
# -> views/.cache/index.html.di (trailing ".div" swapped for ".di",
#    written under a .cache/ directory next to the source rather than
#    beside it -- .cache/ is created automatically if it doesn't exist)

diamond packages/div/bin/divc.di views/index.html.div views/index.html.di
# -> explicit output path, .cache/ convention bypassed entirely
```

Compiled output never sits next to its own `.html.div` source by
default -- a stray generated `.di` file there is easy to mistake for
something hand-written. `require` from your own app code should point at
the `.cache/` path (`require "./views/.cache/index.html"`, not
`require "./views/index.html"`), and `.gitignore` the whole `.cache/`
directory, not an individual-file pattern -- see the repo's own root
`.gitignore` entry for `examples/library/views/.cache/` as a worked
example.

For a clean recursive build of a complete template tree, use the shared
batch driver:

```sh
DIAMOND_BIN=../../build/diamond packages/div/bin/divc_all.sh views
```

It removes `.cache` directories only within the supplied template tree,
then compiles every `*.html.div` file through `divc.di`, including nested
directories. This prevents generated files for renamed or deleted templates
from lingering. Paths containing spaces are supported, and `/` is rejected
as an input to prevent an accidentally broad clean.

The generated function is named after the *input file's own basename*,
not a fixed `render` -- `views/index.html.div` compiles to
`def index_html(...)`, `views/greeting.html.div` to
`def greeting_html(...)`. This matters the moment an app `require`s more
than one compiled template together (a page requiring a partial, say):
`require`'s compile-time expansion merges every required file into one
flat, shared top-level function namespace, so two templates compiled to
the same fixed name would collide. Two different input files that share
a basename in different directories still collide -- a deliberate,
narrow scope cut, not a general namespacing system.

## Tag syntax

```erb
<%# locals: title, items %>
<h1><%= title %></h1>
<ul>
<% items.each() do |item| %>
  <li><%= item %></li>
<% end %>
</ul>
<p>Raw, unescaped: <%== "<b>already-safe html</b>" %></p>
```

| Tag | Meaning |
|---|---|
| `<%# locals: a, b %>` | Once per file; becomes the generated function's own parameter list. Omit it for a zero-arg function. |
| `<%# comment %>` | Dropped from output. |
| `<%= expr %>` | Evaluates `expr`, HTML-escapes it, appends to the output. |
| `<%== expr %>` | Evaluates `expr`, appends **raw** -- no escaping. |
| `<% code %>` | Verbatim statement code (`if`/`each`/`end`, ...) -- no output of its own. |
| anything else | Literal text. |

`<%= %>` escapes `&`, `<`, `>`, `"`, `'` (Rails' own default set) via an
inlined `div_escape_<function-name>` helper written into every generated
file -- see "Why every generated file is self-contained" below.

**Scope cuts, deliberate rather than accidental** (see
[`ROADMAP.md`](ROADMAP.md) for the fuller reasoning):

- No whitespace-trimming tags (`<%- -%>`).
- Tag scanning stops at the first `%>` it finds after a `<%` -- code
  inside a tag that itself contains the literal substring `%>` (inside a
  nested string, say) will terminate that tag early.

## Partials

A compiled template is just an ordinary function, so partial rendering
needs no special mechanism at all -- `require` both compiled files and
call one from the other:

```ruby
# greeting.html.div: <%# locals: name %><%= name %>!
# page.html.div:     <%# locals: name %><p>Hi, <%== greeting_html(name) %></p>

require "./.cache/greeting.html"
require "./.cache/page.html"
puts(page_html("World"))   # => <p>Hi, World!</p>
```

`<%== %>` (raw) is what you want here, not `<%= %>` -- the partial's own
output is already escaped by its own `<%= %>` tags; escaping it again
would double-escape.

## Layouts

A layout is just an ordinary template too -- there's no `yield` keyword
here (Diamond's own `yield` is a hard reserved word, already meaning
"suspend the current Fiber," so it can't be reused as a placeholder
name) and no special compiler support. The child template renders
first, and its output is passed to the layout as an ordinary declared
local, exactly like the `content`/child-rendering pattern under
"Partials" above -- raw (`<%== %>`), for the same already-escaped
reason:

```ruby
# layout.html.div:
# <%# locals: title, content %>
# <html><head><title><%= title %></title></head><body><%== content %></body></html>
#
# index.html.div:
# <%# locals: name %>
# <h1>Welcome, <%= name %></h1>

require "./.cache/layout.html"
require "./.cache/index.html"
puts(layout_html("Home", index_html("World")))
```

`content` is only a naming convention here, not a reserved name --
`<%# locals: %>` accepts any parameter names, so call it whatever reads
best for a given layout.

## Why every generated file is self-contained

Rather than `require`ing a shared runtime module for HTML-escaping, the
translator writes a small `div_escape_<function-name>` helper directly
into every generated file (see `lib/div/compiler.di`'s
`escape_helper_lines`). This keeps a compiled template independently
`require`-able with zero runtime dependency on `packages/div` --
the translator is a build-time-only tool, not something your app needs
installed to actually run. The escaping logic there is kept behaviorally
identical to `Div.escape_html` below; `test.sh` checks the two against
each other directly.

## Runtime helpers (`lib/div/runtime.di`)

```ruby
require "path/to/div/lib/div/runtime"

Div.escape_html(value)                    # same escaping <%= %> uses
Div.html_response(200, body)              # -> [200, {"Content-Type": "text/html"}, body]
Div.html_response(200, body, extra_headers)
```

`Div.html_response` is the whole extent of the Rack integration --
`packages/rack`'s own `[status, headers, body]` convention, with
`Content-Type: text/html` merged in:

```ruby
require "path/to/rack/lib/rack"
require "path/to/div/lib/div/runtime"
require "views/.cache/index.html"   # -> index_html(...)

def app_handler(request, context)
  Div.html_response(200, index_html("Welcome", ["a", "b"]))
end
```

## Tests

```
make test-div-package
```
