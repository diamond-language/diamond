a = [1, 2, 3, 4, 5]

out_of_bounds_rejected = false
begin
  a.slice(6, 1)
rescue error: IndexError
  out_of_bounds_rejected = true
end

negative_length_rejected = false
begin
  a.slice(0, -1)
rescue error: IndexError
  negative_length_rejected = true
end

[a.slice(1, 3), a.slice(3, 100), a.slice(5, 2), a.slice(0, 0), out_of_bounds_rejected, negative_length_rejected]
