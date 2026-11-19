# StringBuilder

[API reference](index.md) · Previous: [LazyEnumerator](lazy_enumerator.md) · Next: [Math](math.md)

A `StringBuilder` collects pieces of text and joins them once at the end, so
building a long `String` in a loop takes time proportional to its length.
`Array#join` does the same for text already in an Array.

```ruby
sb = StringBuilder.new()
3.times() do |i| sb.append(i).append(",") end
sb.to_s()   # => "0,1,2,"
```

## Methods

### `StringBuilder.new() -> StringBuilder`

An empty builder.

### `append(piece) -> StringBuilder`

Adds `piece`, converted to text as interpolation does, and returns the builder
so calls can be chained.

```ruby
StringBuilder.new().append("a").append(1).append(:b).to_s()   # => "a1b"
```

### `length() -> Int`

The length of the text so far, in bytes.

```ruby
StringBuilder.new().append("hé").length()   # => 3
```

### `to_s() -> String`

The text so far.
