# Regression test for this phase's own record_field_known_type fix's
# second bypass site: a struct's generated `initialize` (compile_struct,
# src/compiler.c) also used to emit SET_IVAR without ever touching
# field_type_status/field_known_class, for the same reason attr_
# accessor's own generated writer did (jit_ivar_local_attr_accessor.di's
# own identical regression). A struct field is always explicitly typed
# (required syntax), so this is a real, common shape: any struct field
# read into a local and immediately called on.
struct Box(value: Int)
  def double() -> Int = @value * 2
end

struct Holder(box: Box)
  def run(n) -> Int
    x = @box
    total = 0
    i = 0
    while i < n
      total = total + x.double()
      i = i + 1
    end
    total
  end
end

puts(Holder.new(Box.new(21)).run(50000))
