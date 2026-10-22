def checked_short_circuit(value: String | Nil) -> String
  value != nil && value.length()
  value
end

checked_short_circuit("ok")
