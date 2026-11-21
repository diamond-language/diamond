# String

[API reference](index.md) · Previous: [Object](object.md) · Next: [Array](array.md)

A `String` is an immutable sequence of bytes, usually UTF-8 text. Every method
returns a new value and leaves the receiver unchanged. Positions, lengths and
case conversion work on bytes, so a multi-byte character counts as several
(`"é".length()` is `2`), and `upcase`/`downcase` change ASCII letters only.

Literals are written `"text"`, with `#{expr}` interpolation and backslash
escapes (`\n`, `\t`, `\f`, `\xHH`, `\u{H...}` and the rest, listed in
[core syntax](../core-syntax.md)). Positions start at `0`; where a method takes an index, a
negative one counts from the end.

## Operators

| Operator | Result |
|---|---|
| `a + b` | `a` followed by `b`; `b` must be a `String` |
| `s * n` | `s` repeated `n` times, the same as `repeat(n)` |
| `s % values` | `format(values)` |
| `a == b`, `a != b` | byte-for-byte equality |
| `a < b`, `<=`, `>`, `>=` | byte-wise ordering |
| `s[i]` | the one-byte `String` at `i`; `IndexError` outside the string |
| `s[start, length]` | `slice(start, length)` |
| `s[a..b]`, `s[a...b]` | the bytes in the Range; a negative bound counts from the end |

`s[i] = x` is a `TypeError`: strings cannot be changed in place.

```ruby
"ab" + "cd"          # => "abcd"
"ab" * 3             # => "ababab"
"%.2f" % 3.14159     # => "3.14"
"abc"[-1]            # => "c"
"hello"[1..-1]       # => "ello"
"apple" < "banana"   # => true
```

## Methods

### `between?(min, max) -> Bool`

Whether the receiver sorts between `min` and `max`, both included.

```ruby
"b".between?("a", "c")   # => true
```

### `bytes() -> Array[Int]`

The bytes as an `Array` of Ints from `0` to `255`.

```ruby
"hé".bytes()   # => [104, 195, 169]
```

### `bytesize() -> Int`

The number of bytes. The same as `length()`.

```ruby
"hé".bytesize()   # => 3
```

### `capitalize() -> String`

The first ASCII letter upper case and the rest lower case.

```ruby
"hELLO world".capitalize()   # => "Hello world"
```

### `casecmp?(other) -> Bool`

Whether the two strings are equal when ASCII case is ignored.

```ruby
"ABC".casecmp?("abc")   # => true
```

### `center(width, padding = " ") -> String`

The receiver centered in `width` bytes, padded on both sides with `padding`.
A receiver already `width` long or longer comes back unchanged.

```ruby
"ab".center(6, "*")   # => "**ab**"
```

### `chars() -> Array[String]`

Each byte as a one-byte `String`.

```ruby
"abc".chars()   # => ["a", "b", "c"]
```

### `chomp(suffix = nil) -> String`

Without an argument, removes one trailing `"\n"`, `"\r\n"` or `"\r"`. With
`suffix`, removes it when the receiver ends with it; `chomp("")` removes every
trailing newline.

```ruby
"line\n".chomp()             # => "line"
"report.txt".chomp(".txt")   # => "report"
```

### `chop() -> String`

The receiver without its last byte.

```ruby
"abc".chop()   # => "ab"
```

### `count(chars) -> Int`

How many bytes of the receiver appear in `chars`, read as a set.

```ruby
"hello".count("lo")   # => 3
```

### `deep_freeze() -> String`

The same as `freeze()`; a `String` has nothing inside to freeze.

### `delete(chars) -> String`

The receiver with every byte that appears in `chars` removed.

```ruby
"hello".delete("l")   # => "heo"
```

### `delete_prefix(prefix) -> String`

The receiver without `prefix`, when it starts with it.

```ruby
"v1.2".delete_prefix("v")   # => "1.2"
```

### `delete_suffix(suffix) -> String`

The receiver without `suffix`, when it ends with it.

```ruby
"main.d6".delete_suffix(".d6")   # => "main"
```

### `downcase() -> String`

ASCII letters in lower case.

```ruby
"HeLLo".downcase()   # => "hello"
```

### `dup() -> String`

An unfrozen copy.

### `each_byte() do |byte| ... end -> String`

Calls the block with each byte as an `Int` and returns the receiver.

```ruby
total = 0
"ab".each_byte() do |b| total += b end
total   # => 195
```

### `each_char() do |char| ... end -> String`

Calls the block with each byte as a one-byte `String` and returns the receiver.

```ruby
seen = []
"ab".each_char() do |c| seen.push(c) end
seen   # => ["a", "b"]
```

### `each_line() do |line| ... end -> String`

Calls the block with each line, newline included, and returns the receiver.

```ruby
count = 0
"a\nb\n".each_line() do |line| count += 1 end
count   # => 2
```

### `empty?() -> Bool`

Whether the length is zero.

```ruby
"".empty?()   # => true
```

### `end_with?(suffix, ...) -> Bool`

Whether the receiver ends with any of the given strings.

```ruby
"main.d6".end_with?(".rb", ".d6")   # => true
```

### `equal?(other) -> Bool`

Whether `other` is this same String object; `==` compares the text.

### `escape_html() -> String`

The text with `&`, `<`, `>`, `"` and `'` written as `&amp;`, `&lt;`, `&gt;`, `&quot;` and
`&#39;`, so it is safe inside HTML text, a quoted attribute value, or XML. Div templates
escape `<%= %>` output the same way.

```ruby
"Tom & Jerry's <b>".escape_html()   # => "Tom &amp; Jerry&#39;s &lt;b&gt;"
```

### `format(values) -> String`

`sprintf`-style formatting. `values` is one value or an `Array` of them; `%`
does the same. The directives are `%d`/`%i`, `%f` (with `.N` precision), `%x`,
`%X`, `%o`, `%b`, `%s` and `%%`, with `-` to left-justify and `0` to zero-pad
inside a width. A value of the wrong type for its directive raises
`TypeError`, and too few values raise `ArgumentError`.

```ruby
"Name: %s, Age: %d".format(["Alice", 30])   # => "Name: Alice, Age: 30"
"%05d".format(42)                           # => "00042"
"%-6s|".format("hi")                        # => "hi    |"
```

### `freeze() -> String`

Marks the receiver frozen and returns it. Strings are already immutable, so
this only changes what `frozen?()` reports.

### `frozen?() -> Bool`

Whether `freeze()` was called on the receiver.

```ruby
"abc".freeze().frozen?()   # => true
```

### `getbyte(index) -> Int`

The byte at `index` as an `Int`.

```ruby
"abc".getbyte(0)   # => 97
```

### `gsub(pattern, replacement) -> String`

### `gsub(pattern) do |match| ... end -> String`

Every match of `pattern` (a `Regexp`, or a `String` matched literally)
replaced by `replacement`, in which `\0` is the whole match and `\1`... the
capture groups. With a block, each match is replaced by the block's value.

```ruby
"a.b.c".gsub(".", "-")                    # => "a-b-c"
"2024-01-15".gsub(/(\d+)-(\d+)-(\d+)/, "\\3/\\2/\\1")   # => "15/01/2024"
"a1 b22".gsub(/[0-9]+/) do |n| n.to_i() * 2 end         # => "a2 b44"
```

### `hex() -> Int`

The receiver read as a hexadecimal number.

```ruby
"ff".hex()   # => 255
```

### `include?(needle) -> Bool`

Whether `needle` occurs in the receiver.

```ruby
"hello".include?("ell")   # => true
```

### `index(needle, start = 0) -> Int | Nil`

The position of the first `needle` at or after `start`, or `nil`.

```ruby
"hello".index("l")      # => 2
"hello".index("l", 3)   # => 3
```

### `index_of(needle, start = 0) -> Int | Nil`

The position of the first `needle` at or after byte `start`, or `nil`. Unlike `index`, a
negative `start` finds nothing. The search starts in place, so stepping through a long String
with it costs nothing per call for the text already passed.

```ruby
"hello".index_of("z")      # => nil
"hello".index_of("l", 3)   # => 3
```

### `insert(index, other) -> String`

A new `String` with `other` inserted before `index`.

```ruby
"abc".insert(1, "X")   # => "aXbc"
```

### `inspect() -> String`

The receiver as a quoted literal, with escapes.

```ruby
puts("say \"hi\"".inspect())   # => "say \"hi\""
```

### `length() -> Int`

The number of bytes.

```ruby
"hello".length()   # => 5
```

### `lines() -> Array[String]`

The lines, each with its newline.

```ruby
"a\nb\n".lines()   # => ["a\n", "b\n"]
```

### `ljust(width, padding = " ") -> String`

The receiver padded on the right to `width` bytes. A negative `width` or an
empty `padding` raises `ArgumentError`.

```ruby
"abc".ljust(6, "-")   # => "abc---"
```

### `lstrip() -> String`

The receiver without leading ASCII whitespace.

```ruby
"  hi  ".lstrip()   # => "hi  "
```

### `match(pattern) -> Array | Nil`

Matches `pattern` (a `Regexp`, or a `String` compiled as one). The result is an
`Array` with the whole match at `0` and the capture groups after it, or `nil`
when nothing matched.

```ruby
"id:42".match(/id:([0-9]+)/)   # => ["id:42", "42"]
"abc".match(/z/)               # => nil
```

### `match?(pattern) -> Bool`

Whether `pattern` matches anywhere in the receiver.

```ruby
"x=12".match?(/[0-9]+/)   # => true
```

### `match_data(pattern, position = 0) -> MatchData | Nil`

Like `match`, but returns a [MatchData](match_data.md), with named groups and
offsets, for the first match at or after byte offset `position`.

```ruby
"x=12".match_data("(?<n>[0-9]+)")["n"]   # => "12"
```

### `nil?() -> Bool`

Always `false` for a `String`.

### `oct() -> Int`

The receiver read as an octal number.

```ruby
"17".oct()   # => 15
```

### `ord() -> Int`

The first byte as an `Int`. An empty string raises `IndexError`. `Int#chr` is
the inverse.

```ruby
"A".ord()   # => 65
```

### `partition(separator) -> Array[String]`

`[before, separator, after]` around the first `separator`, or
`[receiver, "", ""]` when it does not occur.

```ruby
"key=value".partition("=")   # => ["key", "=", "value"]
```

### `prepend(other) -> String`

`other` followed by the receiver.

```ruby
"bc".prepend("a")   # => "abc"
```

### `repeat(n) -> String`

The receiver repeated `n` times. A negative `n` raises `RangeError`.

```ruby
"ab".repeat(3)   # => "ababab"
```

### `reverse() -> String`

The bytes in reverse order.

```ruby
"abc".reverse()   # => "cba"
```

### `rindex(needle) -> Int | Nil`

The position of the last `needle`, or `nil`.

```ruby
"hello".rindex("l")   # => 3
```

### `rjust(width, padding = " ") -> String`

The receiver padded on the left to `width` bytes.

```ruby
"7".rjust(3, "0")   # => "007"
```

### `rstrip() -> String`

The receiver without trailing ASCII whitespace.

```ruby
"  hi  ".rstrip()   # => "  hi"
```

### `scan(pattern) -> Array`

Every match of `pattern` (a `Regexp` or a literal `String`). Without capture
groups each element is the matched `String`; with them, each is an `Array` of
that match's captures.

```ruby
"a1 b22 c333".scan(/[0-9]+/)           # => ["1", "22", "333"]
"k1=v1;k2=v2".scan(/([a-z0-9]+)=([a-z0-9]+)/)   # => [["k1", "v1"], ["k2", "v2"]]
```

### `size() -> Int`

The same as `length()`.

### `slice(start, length) -> String`

`length` bytes from `start`, clamped to the end of the string. A `start`
outside the string raises `IndexError`.

```ruby
"hello".slice(1, 3)    # => "ell"
"hello".slice(3, 10)   # => "lo"
```

### `split(separator = nil, limit = nil) -> Array[String]`

The pieces around each `separator`, a `String` or a `Regexp`. Empty pieces
are kept, unlike Ruby. An empty separator splits into one-byte strings, and
no separator splits on runs of whitespace. With `limit`, at most that many
pieces are returned and the last holds the rest.

```ruby
"a,b,,c".split(",")      # => ["a", "b", "", "c"]
"a b  c".split()         # => ["a", "b", "c"]
"a,b,c,d".split(",", 2)  # => ["a", "b,c,d"]
"a1b22c".split(/[0-9]+/) # => ["a", "b", "c"]
```

### `squeeze(chars = nil) -> String`

Runs of the same byte collapsed to one. With `chars`, only bytes in that set
are collapsed.

```ruby
"aaabbb".squeeze()      # => "ab"
"aaabbb".squeeze("a")   # => "abbb"
```

### `start_with?(prefix, ...) -> Bool`

Whether the receiver starts with any of the given strings.

```ruby
"hello".start_with?("he")   # => true
```

### `strip() -> String`

The receiver without leading and trailing ASCII whitespace.

```ruby
"  hi  ".strip()   # => "hi"
```

### `sub(pattern, replacement) -> String`

### `sub(pattern) do |match| ... end -> String`

Like `gsub`, for the first match only.

```ruby
"a-b-c".sub("-", "+")   # => "a+b-c"
```

### `swapcase() -> String`

Upper-case ASCII letters lowered and lower-case ones raised.

```ruby
"Hello".swapcase()   # => "hELLO"
```

### `tap() do |value| ... end -> String`

Calls the block with the receiver and returns the receiver.

### `to_f() -> Float`

The leading decimal number, exponent allowed. Text that is not a number gives
`0.0`, and an out-of-range value gives `Infinity`.

```ruby
"3.5".to_f()   # => 3.5
"1e3".to_f()   # => 1000.0
```

### `to_i(base = 10) -> Int`

The leading integer in `base`. Text that is not a number gives `0`.

```ruby
"42abc".to_i()   # => 42
"ff".to_i(16)    # => 255
```

### `to_s() -> String`

The receiver.

### `to_sym() -> Symbol`

The `Symbol` with this name.

```ruby
"name".to_sym()   # => :name
```

### `tr(from, to) -> String`

Translates each byte found in `from` to the byte at the same position in `to`.
Both sides accept ranges such as `a-z`, and a leading `^` negates `from`. A
shorter `to` repeats its last byte, and an empty `to` deletes the matched
bytes.

```ruby
"hello".tr("el", "ip")     # => "hippo"
"hello".tr("aeiou", "*")   # => "h*ll*"
```

### `unescape_html() -> String`

Decodes character references: numeric ones (`&#233;`, `&#xE9;`) and the named `&amp;`,
`&lt;`, `&gt;`, `&quot;`, `&apos;` and `&nbsp;`. Other named references, and a reference
missing its `;`, are left as they are; an invalid code point becomes U+FFFD.

```ruby
"caf&#233; &amp; &lt;b&gt;".unescape_html()   # => "café & <b>"
```

### `unpack(template) -> Array`

Decodes a binary `String` by `template`, the inverse of `Array#pack`. Integer
directives `C c S s L l Q q` (and `v V`) are little-endian and `n N` are
big-endian; `a A Z` read strings, `H` hex, `U` UTF-8 code points and `x` skips
a byte. A directive takes a count, or `*` for the rest. Reading past the end
gives `nil`.

```ruby
"\x01\x02".unpack("C*")   # => [1, 2]
```

### `unpack1(template) -> Int | String | Nil`

The first value `unpack` would return.

```ruby
"\x00\x01".unpack1("n")   # => 1
```

### `upcase() -> String`

ASCII letters in upper case.

```ruby
"hello".upcase()   # => "HELLO"
```
