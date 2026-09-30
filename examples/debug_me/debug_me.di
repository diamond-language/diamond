# Deliberate bug: three items at 12 cents should total 36, not 15.
def line_total(price, quantity)
  total = price + quantity
  debugger()
  total
end

puts("Total: #{line_total(12, 3)} cents")
exit(0)
