def stop(flag)
  return if flag
  puts("continued")
  7
end

puts(stop(true))
puts(stop(false))
