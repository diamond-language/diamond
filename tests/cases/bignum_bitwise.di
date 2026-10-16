# Bitwise operators, shifts, radix conversion and literals past 64 bits.
big = 18446744073709551615

negative_shift_rejected = false
begin
  1 << (0 - 1)
rescue error: RangeError
  negative_shift_rejected = true
end

oversized_shift_rejected = false
begin
  1 << 2000000
rescue error: RangeError
  oversized_shift_rejected = true
end

[
  big,
  0xFFFF_FFFF_FFFF_FFFF_FFFF,
  0b1_0000000000000000000000000000000000000000000000000000000000000000,
  -9223372036854775808,
  big & 255,
  (big << 4) | 15,
  big ^ big,
  big >> 60,
  1 << 70,
  (1 << 70) >> 70,
  1 << 63,
  -1 << 70,
  (-1 << 70) >> 69,
  (0 - big) & 255,
  (0 - big) | 1,
  (0 - (1 << 70)) >> 3,
  -5 >> 100,
  5 >> 100,
  big.to_s(16),
  (big + 1).to_s(2),
  (0 - big).to_s(36),
  "ffffffffffffffffff".to_i(16),
  "-zz_zzzzzzzzzzzzzzzzzzzz".to_i(36),
  (big * big).to_s(36).to_i(36) == big * big,
  negative_shift_rejected,
  oversized_shift_rejected,
]
