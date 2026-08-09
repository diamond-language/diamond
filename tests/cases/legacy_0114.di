def length_or_zero(value: Sized | Nil) -> Int
 if value is Sized
  value.length()
 else
  0
 end
end
[length_or_zero([1, 2]), length_or_zero(nil)]
