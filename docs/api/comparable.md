# Comparable

[API reference](index.md) · Previous: [Enumerable](enumerable.md) · Next: [LazyEnumerator](lazy_enumerator.md)

`Comparable` is a module that gives a class ordering once it defines `<=>`,
returning a negative, zero or positive `Int`. Including it adds `<`, `<=`,
`>`, `>=` and `==`, plus the methods below.

```ruby
class Version
  include Comparable

  def initialize(major: Int, minor: Int)
    @major = major
    @minor = minor
  end

  def major() = @major
  def minor() = @minor

  def <=>(other)
    return @major <=> other.major() if @major != other.major()
    @minor <=> other.minor()
  end
end

Version.new(1, 2) < Version.new(1, 10)                               # => true
Version.new(2, 0).between?(Version.new(1, 0), Version.new(3, 0))     # => true
```

## Methods

### `between?(min, max) -> Bool`

Whether the receiver is at least `min` and at most `max`.

### `clamp(min, max) -> Value`

`min` when the receiver is below it, `max` when above it, otherwise the
receiver.
