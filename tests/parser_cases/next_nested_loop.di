outer = 0
while outer < 2
  outer = outer + 1
  inner = 0
  loop do
    inner = inner + 1
    if inner == 1
      next
    end
    break
  end
  puts(outer)
end
