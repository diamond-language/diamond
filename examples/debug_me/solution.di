# Multiply unit price by quantity; remove the diagnostic pause.
def line_total(price, quantity)
  total = price * quantity
  total
end

puts("Total: #{line_total(12, 3)} cents")
exit(0)
