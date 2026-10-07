# A function whose body ends in `v is String` used to leave its narrowing behind. The
# `@s?` below parses as the condition of a ternary, lands in the same register, and
# applied the old function's type sets to the top-level function: a segfault in
# the compiler instead of the "instance variable" error.
def s(v: Int | String)
  v is String
end
puts(@s?)
