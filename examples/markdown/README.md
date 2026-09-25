# examples/markdown

A Markdown-to-HTML converter for a practical subset: headings, paragraphs,
bullet and numbered lists, fenced code, nested blockquotes, rules, and
inline `code`, **strong**, *emphasis*, and links. It's a tour of Diamond's
string and `Regexp` handling.

```text
$ diamond markdown.di --toc testdata/sample.md
<nav>
<ul>
  <li><a href="#getting-started">Getting started</a></li>
  <li><a href="#getting-started-2">Getting started</a></li>
    <li><a href="#things-to-try">Things to try</a></li>
</ul>
</nav>
<h1 id="field-notes-diamond-friends">Field Notes: Diamond &amp; Friends</h1>
<p>Diamond is a <em>gradually typed</em> language with <strong>Ruby-like</strong> syntax. ...
```

`testdata/sample.html` has the complete output.

## Usage

```text
markdown [--toc] [--stats] [FILE]
```

Reads `FILE`, or stdin when there is none. `--toc` starts the output with
a table of contents for h2 and h3 headings. `--stats` prints a word count,
a block tally, and a heading outline to stderr. Exits 64 for a usage error
and 66 when the file can't be opened.

## What it shows

- **Regexp as `case` patterns.** `BlockParser#next_block`
  (`lib/blocks.di`) classifies each line with `case line` and `when
  @heading`, `when @fence`, and so on: a `Regexp` in a `when` searches the
  String subject. `match` then pulls out the captures, destructured with
  `[_, hashes, text] = @heading.match(line)`.
- **gsub with backreferences.** `**strong**` and `*emphasis*` become tags
  through `gsub(@strong, "<strong>\\1</strong>")`. The patterns use lazy
  quantifiers and non-capturing groups (reginold compiles Ruby regex
  syntax) so `5 * 3 * 2` isn't mistaken for emphasis.
- **A match loop where a template isn't enough.** Links need a decision per
  match (a `javascript:` URL, matched case-insensitively with option `1`,
  becomes `#`), so `Inline#links` walks the matches with `match`,
  `index_of`, and `slice`, building output with a `StringBuilder`.
- **split as a tokenizer.** Splitting a line on backticks alternates text and
  code, so code spans are escaped but never formatted.
- **Recursion through a sealed tree.** A blockquote's lines are re-parsed by
  a new `BlockParser`, so quotes can hold lists and other quotes. `Block` is
  `sealed`, and `Renderer#block` covers all six kinds.
- **Blocks with `&block`/`yield`.** `take_while(&keep)` consumes lines while
  its block says so; `paragraph_lines` passes one that asks whether any
  block-start pattern matches (`any?`).
- **Attribute declarations.** `attr_reader`, `attr_accessor` (heading ids
  are assigned after parsing), and `attr_predicate ordered: Bool`, which
  defines `ordered?()`.
- **Strings.** Slugs for heading ids come from `downcase` and `gsub`, with
  repeats numbered through a Hash of counts. `--stats` uses `scan` and
  `tally`; errors go to stderr through `warn`, and a file is closed in an
  `ensure`.

## Test

```sh
bash smoke_test.sh
```

This converts `testdata/sample.md` under the interpreter and as a
`diamond build` binary, checks the HTML against `testdata/sample.html`, and
covers stdin input, `--stats`, and the exit codes.
