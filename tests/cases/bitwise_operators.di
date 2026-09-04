shift_overflow_rejected = false
begin
  1 >> 64
rescue error: RangeError
  shift_overflow_rejected = true
end

type_mismatch_rejected = false
begin
  5 & "not an int"
rescue error: TypeError
  type_mismatch_rejected = true
end

[
  5 & 3,
  5 | 2,
  5 ^ 3,
  20 >> 2,
  0 - 8 >> 1,
  5 << 2,
  shift_overflow_rejected,
  type_mismatch_rejected,
]
