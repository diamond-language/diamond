# URI

[API reference](index.md) · Previous: [JSON](json.md)

`URI` percent-encodes text for URLs and reads and writes form bodies and query
strings (`application/x-www-form-urlencoded`). Strings are bytes, so a
multi-byte character becomes one `%XX` per byte, as browsers send it.

```ruby
fields = URI.decode_form("name=Ada+Lovelace&lang=d6")
fields["name"]                              # => "Ada Lovelace"
URI.encode_form({"q": "1 + 1", "page": 2})  # => "q=1+%2B+1&page=2"
```

A request's query string is the part of its path after `?`:

```ruby
path = "/search?q=caf%C3%A9&page=2"
query = path.include?("?") ? path.split("?", 2)[1] : ""
URI.decode_form(query)["q"]   # => "café"
```

## Functions

### `URI.encode_component(text) -> String`

`text` with every byte other than `A`-`Z`, `a`-`z`, `0`-`9`, `-`, `_`, `.`
and `~` written as `%XX`, so it is safe inside a path segment or a query
value.

```ruby
URI.encode_component("a b/c")   # => "a%20b%2Fc"
```

### `URI.decode_component(text) -> String`

Reverses `encode_component`: each `%XX` becomes its byte. A `+` stays a `+`,
and a `%` without two hex digits after it is kept as it is.

```ruby
URI.decode_component("a%20b%2Fc")   # => "a b/c"
```

### `URI.encode_form(fields) -> String`

A form body or query string from a Hash: `name=value` pairs joined by `&`,
with spaces written as `+`. Values are converted with `to_s`, and an Array
value repeats the name once for each element.

```ruby
URI.encode_form({"tag": ["a", "b&c"]})   # => "tag=a&tag=b%26c"
```

### `URI.decode_form(text) -> Hash`

The fields of a form body or query string, as a Hash of Strings. A `+` is a
space, a name with no `=` maps to `""`, and a repeated name keeps its last
value.

```ruby
URI.decode_form("a=1&b&a=2")   # => {"a": "2", "b": ""}
```

### `URI.decode_form_pairs(text) -> Array`

Every `[name, value]` pair in a form body or query string, in order, with
repeated names kept, for fields such as checkboxes that can appear more than
once.

```ruby
URI.decode_form_pairs("a=1&a=2")   # => [["a", "1"], ["a", "2"]]
```
