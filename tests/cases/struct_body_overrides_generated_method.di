# A struct body may replace the generated == and to_s (see
# struct_override_eq_and_to_s), but not a generated field reader: replacing
# `x` would break the field list the rest of the struct relies on.
struct Point(x: Int, y: Int)
  def x() = 99
end
