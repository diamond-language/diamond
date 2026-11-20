# URI

[API reference](index.md) · Previous: [JSON](json.md)

`URI` parses URLs and resolves links against the page they are on, percent-encodes text
for URLs, and reads and writes form bodies and query strings
(`application/x-www-form-urlencoded`). Strings are bytes, so a
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

### `URI.parse(text) -> URI::Reference`

Splits a URI into its parts (RFC 3986): scheme, userinfo, host, port, path, query and
fragment. A relative reference such as `../img/logo.png` or `#top` parses too, with `nil`
for the parts it lacks. The scheme is lower-cased, and everything else is kept as written.
A port that is not a number raises `ArgumentError`.

```ruby
uri = URI.parse("https://example.com:8443/docs/a.html?lang=d6#top")
[uri.host(), uri.port(), uri.path(), uri.query(), uri.fragment()]
# => ["example.com", 8443, "/docs/a.html", "lang=d6", "top"]
```

### `URI.join(base, reference) -> String`

`reference` resolved against `base`, as a browser resolves a link on the page at `base`
(RFC 3986 section 5.2). Short for `URI.parse(base).resolve(reference).to_s()`.

```ruby
URI.join("https://example.com/docs/guide/", "../api/uri.html")   # => "https://example.com/docs/api/uri.html"
URI.join("https://example.com/docs/a.html", "#top")              # => "https://example.com/docs/a.html#top"
```

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

## URI::Reference

What `URI.parse` returns. Interpolation and `to_s` write it back out, and `==` compares that
text.

### `scheme() -> String | Nil`

### `userinfo() -> String | Nil`

### `host() -> String | Nil`

### `port() -> Int | Nil`

### `path() -> String`

### `query() -> String | Nil`

### `fragment() -> String | Nil`

The parts. Each is `nil` when absent, except the path, which is `""` when empty. `port` is
the port written in the URI; `default_port` gives the scheme's.

### `absolute?() -> Bool`

Whether there is a scheme. `"//host/a"` and `"../a"` are not absolute.

### `default_port() -> Int | Nil`

80 for `http` and `ws`, 443 for `https` and `wss`, otherwise `nil`.

### `origin() -> String | Nil`

`scheme://host[:port]`, with the host lower-cased and the port only when it is not the
default; `nil` without a scheme and host. Rate limits and robots.txt apply per origin.

### `request_target() -> String`

The path and query as an HTTP request line has them, with `/` for an empty path.

### `resolve(reference) -> URI::Reference`

`reference`, a String or `URI::Reference`, resolved against this one as its base.

```ruby
page = URI.parse("https://example.com/docs/guide/basics.html")
page.resolve("types.html").to_s()      # => "https://example.com/docs/guide/types.html"
page.resolve("/").to_s()               # => "https://example.com/"
page.resolve("//cdn.example/x.js").to_s()   # => "https://cdn.example/x.js"
```

### `normalize() -> URI::Reference`

A canonical copy for comparing links: scheme and host lower-cased, the default port
dropped, `.` and `..` segments removed, and `/` for an empty path after a host.
Percent-encoding is left alone.

```ruby
URI.parse("HTTP://Example.com:80/a/./b/../c").normalize().to_s()   # => "http://example.com/a/c"
```

### `with_fragment(fragment) -> URI::Reference`

A copy with another fragment; `nil` removes it.

### `to_s() -> String`

The reference as text.

### `inspect() -> String`

`#<URI::Reference ...>`.
