arr = [1, 2, 3]
arr.freeze()
puts(arr.frozen?())
begin
  arr.push(4)
rescue error: FrozenError
  puts("push blocked")
end
begin
  arr[0] = 99
rescue error: FrozenError
  puts("[]= blocked")
end
begin
  arr.pop()
rescue error: FrozenError
  puts("pop blocked")
end
begin
  arr.delete_at(0)
rescue error: FrozenError
  puts("delete_at blocked")
end
puts(arr.length())
puts(arr[0])
