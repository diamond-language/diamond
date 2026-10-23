# Falsy block results, slice and radix arguments, exact logarithms and cube roots, and Int divmod past 2**53.
def raises_argument_error(index)
  begin
    if index == 0 then [1].each_slice(0)
    elsif index == 1 then [1].each_slice(-2)
    elsif index == 2 then 10.digits(1)
    else 10.digits(0)
    end
    "no error"
  rescue e: ArgumentError
    "ArgumentError"
  end
end

errors = []
index = 0
while index < 4
  errors.push(raises_argument_error(index))
  index += 1
end

[[1].all?() do |x| nil end,
 [1, 2].reject() do |x| nil end,
 [1, 2, 3].reject() do |x| x > 1 end,
 {"a": 1}.all?() do |k, v| nil end,
 errors,
 [1, 2, 3].each_slice(2),
 Math.log10(1000), Math.log10(5) < 0.7, Math.log2(8), Math.cbrt(27), Math.cbrt(-8),
 9223372036854775807.divmod(10), (-7).divmod(2), 7.divmod(-2), 7.5.divmod(2)]
