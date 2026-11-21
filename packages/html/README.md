# html

Parses HTML into a tree of nodes and finds elements in it with CSS selectors.
Needs d6 1.2.0 or later. Before d6 1.3.0, parse time grows faster than page size,
so a megabyte page takes seconds.

```ruby
require "html"

doc = HTML.parse(File.read("page.html"))
puts(doc.title())
doc.select("a[href]").each() do |link|
  puts("#{link["href"]}  #{link.text().strip()}")
end
heading = doc.at("main h1")
puts(heading.text()) if heading != nil
```

`HTML.parse` never raises. Like a browser, it reads any input and closes whatever the markup left open.

## The tree

`HTML.parse(text)` returns an `HTML::Document`. Its children are what the markup has at the top level. It adds no
`<html>`, `<head>` or `<body>` that the markup didn't write. Nodes are:

| Class | What it is |
|---|---|
| `HTML::Element` | `name` (lowercase), `attributes` (a `Hash`, lowercase names, decoded values), `children` |
| `HTML::Text` | `content`, with character references decoded |
| `HTML::Comment` | `content`, the text between `<!--` and `-->` |
| `HTML::Doctype` | `content`, what follows `<!DOCTYPE`, such as `"html"` |

Every node has `parent`, `next_sibling`, `previous_sibling`, `text` and `to_html`.

An element also has:

- `element["href"]` reads an attribute (`nil` when it's missing), and `element["href"] = "/x"` sets one.
- `has_attribute?`, `id` and `classes`, plus `has_class?("x")`.
- `elements` gives the child elements, and `descendants` every element inside, in document order.
  `each_element() do |e| ... end` walks them with a block.
- `select("css")` returns every matching element inside, `at("css")` the first one or `nil`, and
  `matches?("css")` tests the element itself.
- `inner_html` serializes the children, and `to_html` the element with them.
- `append(node)` adds a child and `remove(node)` takes one out.

The document also has `doctype`, `root` (the first top-level element) and `title`, which is the stripped
text of the first `<title>`.

`HTML.decode(text)` decodes character references on their own: every HTML 4 named reference, HTML5's
ASCII punctuation names, and decimal and hex references.

## Selectors

| Selector | Matches |
|---|---|
| `p`, `*` | tag, any element |
| `#main`, `.note` | id, class |
| `[href]`, `[lang=en]` | has the attribute, value equals |
| `[rel~=nofollow]`, `[lang\|=en]` | word in a space-separated list; `en` or `en-...` |
| `[href^=https]`, `[href$=".pdf"]`, `[href*=example]` | starts with, ends with, contains |
| `[type=TEXT i]` | the value compared case-insensitively |
| `:first-child`, `:last-child`, `:only-child` | position among the sibling elements |
| `:nth-child(2n+1)`, `:nth-last-child(odd)` | `an+b`, `odd`, `even` or a number |
| `:empty`, `:root`, `:not(sel, ...)` | no child elements or text; top level; doesn't match |
| `a b`, `a > b`, `a + b`, `a ~ b` | descendant, child, next sibling, later sibling |
| `a, b` | either |

A selector it can't read raises `HTML::SelectorError` (an `ArgumentError`), naming the position.

## How it parses

The tokenizer follows browsers:

- Comments, doctypes and `<?...?>` are read. A `<` that starts no tag stays text.
- Attributes can be quoted or unquoted. When an attribute repeats, the first value wins.
- `<script>`, `<style>` and the other raw text elements run to their end tag, whatever they hold.
- `<title>` and `<textarea>` work the same way but decode character references.
- Line endings become `\n`.

The tree follows the end-tag rules that matter on real pages:

- Void elements (`<br>`, `<img>` and the rest) never hold content.
- A block element closes an open `<p>`.
- `<li>`, `<dt>`/`<dd>`, `<option>`, table rows and cells, and headings close an open sibling.
- An end tag that matches no open element is ignored.
- `/>` closes an element only inside `<svg>` and `<math>`.

It is not the full HTML5 tree-construction algorithm. Misnested formatting tags (`<b><i></b></i>`) are closed,
not reopened. Content after `</body>` stays where the markup puts it.
