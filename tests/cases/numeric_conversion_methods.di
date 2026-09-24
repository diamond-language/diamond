def typed_round(x: Float) -> Int
  x.round() + 1
end
big = 2 * 9223372036854775807
nan_error = begin
  (0.0 / 0.0).floor()
rescue error: RangeError
  error.message()
end
[42.to_s(), (0 - 7).to_s(), 2.5.to_s(), 3.0.to_s(), 2.345.round(), 2.345.round(1), 2.345.round(2),
 1234.5.round(-2), 2.5.round(), 2.7.floor(), 2.1.ceil(), 2.9.to_i(), 7.abs(), 1.5.abs(),
 3.to_f(), 3.to_i(), typed_round(1.6), 1.0e20.round(), big.to_s(), big.abs() == big, nan_error]
