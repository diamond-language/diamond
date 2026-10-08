# div

Compile `.html.div` view templates into ordinary Diamond functions. A template is
HTML with embedded Diamond; the compiled result is a function you call with the
template's locals and that returns the rendered string. Generated code has no
runtime dependency on this cut.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add div --registry https://cuts.dilang.tech --version "^0.2.2"
facet update
```

This installs the cut into `cuts/div/`. `require_cut "div"` loads the small runtime
helpers (`Div.html_response`, `Div.escape_html`, `Div.hidden_field_tag`); the compiler
itself runs from the command line.

## Using it in a project

A typical layout keeps templates under `views/` and application code beside it:

```
myapp/
  diamond.cut
  cuts/div/ cuts/http/        # installed by facet
  app.di
  views/
    layout.html.div
    notes/
      _note.html.div
      index.html.div
```

**1. Write templates.** The first line declares the template's parameters:

```erb
<%# views/notes/index.html.div %>
<%# locals: notes %>
<h1>Notes</h1>
<ul>
<% notes.each() do |note| %>
  <%== notes__note_html(note) %>
<% end %>
</ul>
```

```erb
<%# views/notes/_note.html.div %>
<%# locals: note %>
<li><%= note["title"] %></li>
```

```erb
<%# views/layout.html.div %>
<%# locals: title, content %>
<!doctype html>
<html>
  <head><title><%= title %></title></head>
  <body>
    <%== content %>
  </body>
</html>
```

**2. Compile the whole tree.** `divc_all.sh` recompiles every `*.html.div` below a
directory into a `.cache/` directory beside each source file:

```sh
cuts/div/bin/divc_all.sh views
# compiled 3 templates under /…/myapp/views
```

It wipes and rebuilds every `.cache/` under that directory each run, so deleted
templates do not linger. Set `DIAMOND_BIN` if `diamond` is not on `PATH`. Add
`.cache/` to `.gitignore` and run this step from your build script, deploy script,
or a `make views` target. Generated files are not meant to be edited or committed.

**3. Require the generated files and call them.**

```ruby
# app.di
require_cut "div"
require_cut "http"
require "./views/.cache/layout.html"
require "./views/notes/.cache/_note.html"
require "./views/notes/.cache/index.html"

NOTES = [{"title": "Buy milk"}, {"title": "<b>escaped</b>"}]

def handler(request)
  if request["path"] == "/notes"
    body = layout_html("Notes", notes_index_html(NOTES))
    Div.html_response(200, body)
  else
    [404, {"Content-Type": "text/plain"}, "not found"]
  end
end

http_serve(8080, handler)
```

```sh
diamond app.di
curl localhost:8080/notes
```

## Template syntax

| Tag | Meaning |
|---|---|
| `<%# locals: a, b %>` | Once per file. Becomes the render function's parameter list. |
| `<%= expr %>` | Output `expr`, HTML-escaped. |
| `<%== expr %>` | Output `expr` unescaped. Use only for trusted HTML, such as another template's result. |
| `<% code %>` | Run Diamond code; no output. Blocks (`each ... do`, `if`) span tags. |
| `<%# text %>` | Comment; dropped from the output. |

There is no layout or `yield` mechanism: a compiled template is a plain function,
so a layout, a partial, and a page are just functions calling each other, as above.
Tag scanning stops at the first `%>`, so a tag whose code contains that text inside
a string ends early.

## Generated function names

Every template becomes one top-level function. All required files share one flat
namespace, so `divc_all.sh` qualifies the name by the template's path under the
directory you gave it, replacing every character other than letters, digits, and
`_` with `_`, and dropping the `.div` suffix:

| Template | Function |
|---|---|
| `views/layout.html.div` | `layout_html` |
| `views/notes/index.html.div` | `notes_index_html` |
| `views/notes/_note.html.div` | `notes__note_html` |

Compiling a single file with `divc.di` (below) uses its bare file name instead, so
two same-named templates from different directories would collide.

## Compiling one file

```sh
diamond cuts/div/bin/divc.di views/index.html.div
# Writes views/.cache/index.html.di
```

An optional second argument gives an explicit output path, and an optional third
gives the name path used for the function name.

## Helpers

- `Div.html_response(status, body, headers = {})` returns `[status, headers, body]`
  with `Content-Type: text/html`, the response shape `http_serve` and `rack` expect.
- `Div.escape_html(value)` is the same escaping `<%= %>` applies.
- `Div.hidden_field_tag(name, value)` returns an `<input type="hidden">` tag with both
  attributes escaped, for CSRF tokens: `<%== Div.hidden_field_tag("csrf_token", token) %>`.
