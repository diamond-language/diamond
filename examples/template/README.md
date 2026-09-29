# examples/template

A small Mustache-style template renderer over a JSON context.

```text
$ diamond template.di testdata/invoice.mustache testdata/invoice.json
Invoice for Ada Lovelace

  2x widget @ $5

  1x gadget @ $12

Total: $22

Balance due.

Raw HTML note: <b>call before shipping</b>
Escaped note: &lt;b&gt;call before shipping&lt;/b&gt;
```

```text
Invoice for {{customer.name}}

{{#items}}
  {{quantity}}x {{name}} @ ${{price}}
{{/items}}
{{^items}}
  (no items)
{{/items}}
```

## Usage

```text
template.di TEMPLATE_FILE CONTEXT_FILE.json
```

Renders `TEMPLATE_FILE` against the JSON object in `CONTEXT_FILE.json` and
writes the result to stdout. Exit status is 0 on success, 64 for a usage
error, 65 for a bad template (a mismatched, unmatched, or missing closing
tag, reported with its position) or invalid JSON, and 66 when a file
can't be opened.

## Syntax

- `{{path}}` — HTML-escaped; `{{{path}}}` — the same value, unescaped.
- `{{#path}}...{{/path}}` — a section: repeats its body once per element
  if `path` is an Array, once (with that value as the new context, if
  it's a Hash) for any other truthy value, or not at all for
  `false`/`nil`/an empty Array.
- `{{^path}}...{{/path}}` — the inverse: renders exactly when `{{#path}}`
  wouldn't.
- `{{!comment}}` — dropped.
- `path` is dotted (`customer.name`) to reach into a nested Hash, or `.`
  for the current context itself (useful inside a section over an array
  of plain values rather than an array of Hashes). A path that doesn't
  resolve reads as `nil` rather than raising.

## What it shows

- **A one-pass parser that builds the final tree directly**
  (`lib/parser.di`): no separate flat-token pass before nesting — each
  open section pushes a frame (its own path, `^`/`#` flag, and
  body-so-far) onto a stack, and its matching close pops that frame
  straight into its parent's body as a finished `SectionNode`. A
  mismatched close, an unmatched one, or one left open at end of input is
  a `TemplateError` naming the position.
- **A sealed 3-node tree** (`lib/nodes.di`): `TextNode`, `VarNode`,
  `SectionNode`. `render_nodes`'s `case` has no `else` — a fourth kind
  stops it compiling until handled.
- **Mutual recursion**: `render_section` calls `render_nodes` for its
  body, which calls `render_section` again for any section nested inside
  — as many levels deep as the template actually nests, with no
  size limit baked into the renderer itself.
- **A JSON value is Diamond's own value**: `JSON.parse` already produces
  the `Hash`/`Array`/`String`/`Float`/`Bool`/`Nil` this needs, so the
  whole context-lookup layer (`lib/render.di`) is under 20 lines, and
  `template_truthy?`'s three cases are exactly JSON's own falsy shapes
  plus Diamond's own empty-`Array` convention.
- **`gsub` needs a `Regexp`, even for a literal string**: `template_
  escape_html` builds one per character it replaces, rather than a
  string-literal `gsub` overload that doesn't exist.

## Test

```sh
bash smoke_test.sh
```

Runs the program under the interpreter and as a `diamond build` binary,
and checks the rendered invoice and every error path.
