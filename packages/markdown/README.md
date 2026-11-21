# markdown

Renders Markdown to HTML. It is the renderer the [d6 documentation](https://rolld6.codes/docs/)
is built with, so it covers what technical writing uses rather than all of CommonMark.
Needs d6 1.0.1 or later.

```ruby
require_cut "markdown"

puts(Markdown.render("# Notes\n\nSome *emphasis* and `code`."))
# <h1 id="notes"><a class="anchor" href="#notes">Notes</a></h1>
# <p>Some <em>emphasis</em> and <code>code</code>.</p>
```

## Installation

```sh
facet add markdown
facet install
```

## What it renders

- ATX headings (`#` to `######`). Each gets an id made the way GitHub makes one (lower case,
  punctuation dropped, spaces to hyphens, `-1`, `-2` for repeats) and a self-link
  `<a class="anchor">` around its text.
- Paragraphs, block quotes and horizontal rules.
- Bullet and ordered lists, nested by indentation. A blank line between items makes the list
  loose (items keep their `<p>`); an ordered list that starts past 1 keeps its number.
- Fenced code (```` ``` ```` or `~~~`), with `class="language-NAME"` from the fence.
- Tables with `:---`, `---:` and `:---:` alignment, and `\|` for a literal bar.
- Inline code (any number of backticks), `[links](url)`, `<https://autolinks>`, `**strong**`,
  `*emphasis*`, `_emphasis_` and backslash escapes.
- Raw HTML blocks pass through as written. HTML comments are dropped. In running text only
  `<a ...>`, `</a>`, `<br>`, `<kbd>`, `<sup>` and `<sub>` are kept; every other `<` is
  escaped, so text such as `Array<String>` stays readable.

It does not do reference-style links, setext (underlined) headings, indented code blocks,
images or footnotes.

## Options

`Markdown::Renderer.new(...)` takes keyword options, all optional, and renders as many
documents as you give it:

```ruby
require_cut "markdown"

renderer = Markdown::Renderer.new(
  links: {"guide.md" => "/guide/"},             # links to these files go to these URLs
  fallback: "https://github.com/me/site/blob/main/",   # prefix for other relative links
  language_aliases: {"rb" => "ruby"},           # ```rb blocks get class="language-ruby"
  code_markers: {"no-run" => "data-run=\"no\""}, # <!-- no-run --> above a fence adds this
  table_class: "table-wrap"                     # wrap each <table> in <div class="table-wrap">
)
page = renderer.render("# Guide\n\nSee [the guide](guide.md#setup).\n\n## Setup\n")
page.html()       # the HTML body
page.title()      # => "Guide", the first "#" heading ("" when there is none)
page.headings()[0].id()   # => "setup", one Markdown::Heading per "##" and "###"
```

A mapped link keeps its `#fragment`. `fallback` leaves `#fragment`-only links, absolute
paths (`/x`) and URLs with a scheme alone.

## API

- `Markdown.render(source) -> String`
- `Markdown::Renderer.new(links:, fallback:, language_aliases:, code_markers:, table_class:)`
- `Markdown::Renderer#render(source) -> Markdown::Document`
- `Markdown::Document`: `html()`, `title()`, `headings()`
- `Markdown::Heading`: `level()`, `id()`, `text()`, for every `##` and `###` heading
