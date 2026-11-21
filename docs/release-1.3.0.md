# d6 1.3.0

1.3.0 adds what a link checker ran short of: a program that fetches pages, finds their links
and follows them. Programs, projects and cuts that run on 1.2.0 run the same on 1.3.0
([compatibility](compatibility.md)). Everything new here, heredocs and the extra string
escapes included, was a compile error before.

## Heredocs

`<<~NAME` takes the lines up to `NAME` as a String, with the indentation they share removed,
so a page template or SQL query can sit indented with the code around it. Escapes and `#{}`
work as in `"..."`; `<<~'NAME'` is raw. The REPLs, the LSP formatter and every highlighter
know them ([core syntax](core-syntax.md#heredocs)).

```ruby
page = <<~HTML
  <p class="greeting">
    Hello, #{name}!
  </p>
HTML
```

## URLs and HTML

- `URI.parse(text)` returns a `URI::Reference` with the RFC 3986 parts (`scheme`, `host`,
  `port`, `path`, `query`, `fragment` and the rest), plus `resolve`, `normalize`, `origin` and
  `request_target`. `URI.join(base, reference)` resolves a link the way a browser does
  ([URI](api/uri.md)).
- The new `html` cut: `HTML.parse` reads any page, however malformed, into a tree of
  elements, text and comments. `select`, `at` and `matches?` find elements with CSS selectors
  ([README](https://github.com/diamond-language/d6/blob/main/packages/html/README.md)).
- `String#escape_html` and `String#unescape_html`.

## Text

- `Regexp.escape(text)` quotes a String for use inside a pattern.
- String literals accept `\a`, `\b`, `\f`, `\v`, `\s` and `\'`.
  [Core syntax](core-syntax.md) lists every escape.
- `String#index_of(needle, start)` searches from a byte position.
- `String#index(needle, start)` searches in place. It copied the rest of the String on every
  call, so a scanner stepping through a long page took quadratic time.
- Ruby's `rescue => e` and `rescue Type => e` now fail with an error naming d6's `rescue e`
  and `rescue e: Type`, instead of a confusing one about the `=>`.

## Processes

`Process.executable()` returns the path of the running d6, so a script can start another
script with the same interpreter.

## Projects and cuts

- `facet` writes `Cutfile.lock` as plain JSON. It used to have a trailing comma, so
  `JSON.parse` rejected it. Older lockfiles still install.
- `facet new` app templates start their Cutfile at version 0.1.0, and the cli template
  prints it for `--version`.
- New cut on [cuts.rolld6.codes](https://cuts.rolld6.codes): `html` 0.1.0.
- Updated cut: http 0.2.7. A relative redirect `Location` resolves against the request. A
  body without a length stops at the size limit, which `max_body_bytes` sets per request.
  `http_serve` takes `host`, a `ready` Channel (so port 0 works) and a `stop` Channel.

## Installing

[Getting started](getting-started.md#1-install) compares the ways to install d6. Packages
and tarballs are on the [release page](https://github.com/diamond-language/d6/releases/tag/v1.3.0).
