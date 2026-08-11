def selected(value: Int, enabled: Bool) -> Int | Nil = value if enabled

puts(selected(47, true))
puts(selected(47, false))
