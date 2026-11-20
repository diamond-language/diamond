# MatchData

[API reference](index.md) · Previous: [Regexp](regexp.md) · Next: [Process, Signal, streams and IO](process.md)

A `MatchData` is one match, from `Regexp#match_data` or `String#match_data`.
Group `0` is the whole match and the capture groups follow it. A group is
reached by number, by name or by Symbol. Offsets are byte offsets into the
matched String, the units `String#slice` uses.

```ruby
m = /(?<key>\w+)=(?<value>\w+)/.match_data("set color=blue now")
m[0]               # => "color=blue"
m["value"]         # => "blue"
m[:key]            # => "color"
m.offset(0)        # => [4, 14]
m.post_match()     # => " now"
m.named_captures() # => {"key": "color", "value": "blue"}
```

## Methods

### `[](group) -> String | Nil`

A group's text. `group` is an Int (negative counts from the end), or a group's
name as a String or Symbol. Returns `nil` for a group that took no part or a
number out of range; an unknown name raises `IndexError`. When several groups
share a name, the last one that took part answers.

### `offset(group) -> Array | Nil`

A group's `[begin, end]` byte offsets, or `nil` when it took no part.

### `pre_match() -> String`

The text before the match.

### `post_match() -> String`

The text after the match.

### `to_a() -> Array`

Every group's text, the whole match first.

### `captures() -> Array`

The capture groups' text, without the whole match.

```ruby
/(\d+)-(\d+)/.match_data("ids 4-9").captures()   # => ["4", "9"]
```

### `names() -> Array`

The pattern's group names.

### `named_captures() -> Hash`

Each name with its group's text.

### `size() -> Int`

### `length() -> Int`

The number of groups, counting the whole match.

### `to_s() -> String`

The whole match.

### `string() -> String`

The String that was searched.

### `regexp() -> Regexp`

The pattern that matched.

### `new(regexp, string, spans, group_names)`

Built by `match_data`; there is no reason to call it directly.
