# Regexp

[API reference](index.md) · Previous: [Time](time.md) · Next: [MatchData](match_data.md)

A `Regexp` is a compiled regular expression in Ruby syntax. Write one as a
literal, `/pattern/flags`, or build it from a String with `Regexp.new`. The
flags are `i` (ignore case), `m` (`.` matches newline) and `x` (ignore
whitespace and `#` comments in the pattern). Literals do not interpolate. A
Regexp prints as its literal, `/pattern/flags`.

`match` returns the groups as an Array; `match_data` returns a
[MatchData](match_data.md) with named groups and positions. `String#match`,
`match?`, `match_data`, `scan`, `split`, `sub` and `gsub` also accept a Regexp
(see [String](string.md)). There is no `=~`.

```ruby
re = /(\d+)-(\d+)/
re.match("id:42-99")      # => ["42-99", "42", "99"]
re.match?("no digits")    # => false
```

## Creating

### `Regexp.new(pattern, options = 0) -> Regexp`

Compiles `pattern`. `options` is a bitmask: `1` ignores case, `2` lets `.`
match a newline and `4` is extended mode; combine them with `|`. An invalid
pattern raises `RegexpError`.

```ruby
Regexp.new("hello", 1).match?("HELLO")   # => true
Regexp.new("a/b", 1 | 2).to_s()          # => "/a\\/b/mi"
```

### `Regexp.escape(text) -> String`

`text` with every metacharacter backslashed, so a pattern built from it matches the text
literally. Tabs, newlines and the other whitespace escapes come back as `\t`, `\n`, `\r`,
`\f` and `\v`, and a space as `\ ` (so it survives extended mode).

```ruby
Regexp.escape("1+1=2?")                                 # => "1\\+1=2\\?"
Regexp.new("^" + Regexp.escape("a.b") + "$").match?("axb")   # => false
```

## Methods

### `match(text) -> Array | Nil`

The first match in `text`: an Array with the whole match at `0` and each
capture group after it (`nil` for a group that did not take part), or `nil`
when there is no match.

```ruby
/(a)|(b)/.match("b")   # => ["b", nil, "b"]
```

### `match?(text) -> Bool`

Whether the pattern matches anywhere in `text`, without collecting captures.

```ruby
/\d/.match?("a1")   # => true
```

### `match_data(text, position = 0) -> MatchData | Nil`

The first match starting at or after byte offset `position`, as a
[MatchData](match_data.md), or `nil` when there is none.

```ruby
m = /(?<year>\d+)-(?<month>\d+)/.match_data("released 2026-11")
m["year"]       # => "2026"
m.pre_match()   # => "released "
/\d/.match_data("a1b2", 2)[0]   # => "2"
```

### `names() -> Array`

The names of the named groups, in order.

```ruby
/(?<year>\d+)-(?<month>\d+)/.names()   # => ["year", "month"]
```

### `source() -> String`

The pattern text, without slashes or flags.

```ruby
/a+b/i.source()   # => "a+b"
```

### `options() -> Int`

The option bits, as `Regexp.new` takes them.

```ruby
/a/mi.options()   # => 3
```
