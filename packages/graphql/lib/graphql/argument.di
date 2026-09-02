module GraphQL

  # A field or directive argument, or (reused as-is, see
  # InputObjectType) one entry of an input object type's own fields.
  #
  # `has_default` is a separate flag from `default_value`, not folded
  # into "default_value == nil means no default" -- GraphQL itself
  # distinguishes "no default was given" from "the default is literally
  # null" (an argument declared `arg: String = null` is different from
  # one declared `arg: String` with no `= ...` clause at all: the first
  # has a default, and that default happens to be null; the second has
  # no default, so omitting it entirely leaves it unset rather than
  # null). Collapsing these onto Diamond's own `nil` would lose that
  # distinction.
  #
  # Positional constructor, no keyword-call syntax available here --
  # `ClassName.new(x: 1)` stays positional-only in Diamond (confirmed
  # against docs/syntax.md's own note on this), so every builder class in
  # this package takes required fields first, then optional ones with
  # defaults, in this same style.
  class Argument
    attr_reader name
    attr_reader type
    attr_reader default_value
    attr_reader description

    def initialize(name, type, default_value = nil, has_default = false, description = nil)
      @name = name
      @type = type
      @default_value = default_value
      @has_default = has_default
      @description = description
    end

    def has_default?() = @has_default
  end

end
