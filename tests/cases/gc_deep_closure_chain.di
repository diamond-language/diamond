# A chain of closures, each capturing the previous one, is 150000 deep: GC
# marking walks closure -> captured cell -> closure iteratively, not recursively.
def link(prev)
  def get()
    prev
  end
  get
end

f = nil
150000.times() do |i|
  f = link(f)
end
walked = 0
cursor = f
while cursor != nil
  cursor = cursor()
  walked += 1
end
puts("closure chain: #{walked}")
