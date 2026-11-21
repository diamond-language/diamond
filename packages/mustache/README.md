# mustache

[Mustache](https://mustache.github.io/mustache.5.html) templates, read and rendered while the
program runs, for templates that are data rather than code: layouts a site generator finds in
its input, emails an operator edits, or text a user supplies. Needs d6 1.2.0 or later.

```ruby
require_cut "mustache"

page = Mustache.render("<h1>{{title}}</h1>\n{{#posts}}\n<p>{{name}}</p>\n{{/posts}}\n",
  {"title" => "Notes & more", "posts" => [{"name" => "One"}, {"name" => "Two"}]})
puts(page)
# <h1>Notes &amp; more</h1>
# <p>One</p>
# <p>Two</p>
```

For templates written in the program itself, the `div` cut compiles them ahead of time
into d6 functions instead.

## Installation

```sh
facet add mustache
facet install
```

## Tags

| Tag | Renders |
|---|---|
| `{{name}}` | the value, HTML-escaped (`&`, `<`, `>`, `"`); nothing for a missing name or `nil` |
| `{{{name}}}`, `{{&name}}` | the value as written |
| `{{#name}}...{{/name}}` | once per item of an Array, once for any other true value, never for `nil`, `false` or an empty Array, String or Hash |
| `{{^name}}...{{/name}}` | once when `{{#name}}` would not render |
| `{{>name}}` | the partial `name`, with the same values |
| `{{! text }}` | nothing |

Inside a section, names are looked up in the section's value first (a Hash, or the Array
item), then outwards to the data passed in. `{{a.b}}` finds `a` that way and then `b`
inside it, and `{{.}}` is the current value itself, such as each String of an Array.

A section, inverted section, comment or partial tag on a line of its own removes the
whole line, so a template laid out one tag per line leaves no blank lines behind.

Changing delimiters (`{{=<% %>=}}`) and lambdas are not supported.

## Parsing once

`Mustache::Template.new(source, name, partials)` parses a template; `render(data)` fills it
in, as many times as needed. `name` appears in errors. Partials are template source
Strings (parsed the first time they are used) or `Mustache::Template`s.

```ruby
require_cut "mustache"

item = "<li>{{name}}</li>"
list = Mustache::Template.new("<ul>{{#items}}{{>item}}{{/items}}</ul>", "list", {"item" => item})
list.render({"items" => [{"name" => "a"}, {"name" => "b"}]})   # => "<ul><li>a</li><li>b</li></ul>"
```

A template that does not parse raises `Mustache::Error` with the name and line, and so does a
missing partial when the template renders:

```ruby
require_cut "mustache"

begin
  Mustache::Template.new("{{#posts}}\n{{/post}}", "layouts/list.html")
rescue error: Mustache::Error
  error.message()   # => "layouts/list.html:2: {{/post}} closes {{#posts}} from line 1"
end
```

## API

- `Mustache.render(source, data, partials = {}) -> String`
- `Mustache::Template.new(source, name = "template", partials = {})`
- `Mustache::Template#render(data) -> String`
- `Mustache::Error`
