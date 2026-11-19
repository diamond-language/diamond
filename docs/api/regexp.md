# Regexp

[API reference](index.md) · Previous: [Time](time.md)

A `Regexp` is a compiled regular expression in Ruby syntax. Write one as a
literal, `/pattern/flags`, or build it from a String with `Regexp.new`. The
flags are `i` (ignore case), `m` (`.` matches newline) and `x` (ignore
whitespace and `#` comments in the pattern). Literals do not interpolate.

Besides the two methods below, `String#match`, `match?`, `scan`, `split`,
`sub` and `gsub` accept a Regexp (see [String](string.md)). There is no `=~`
and no MatchData object: a match is an Array.

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
