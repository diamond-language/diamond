# diamond_format_value_type had no branch for DIAMOND_VALUE_CLASS: the
# class-index byte DIAMOND_CLASS() (value.h) stores shares the same union
# slot the function's object-kind fallback reads as `value.as.object`, so
# exit_helper's own "exit code must be an Int, got %s" formatting
# dereferenced that small integer as a pointer and segfaulted instead of
# reporting the type -- reachable from ordinary Diamond source (`self`
# inside a `def self.x` singleton method already is a class value), not
# just adversarial bytecode.
class Foo
  def self.bar()
    self
  end
end

begin
  exit(Foo.bar())
rescue error: TypeError
  puts(error.message())
end
