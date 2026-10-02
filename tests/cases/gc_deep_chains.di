# The collector marks live data without using one C stack frame per nesting
# level. Each chain below is far deeper than the point where recursive
# marking used to overflow the stack (a few tens of thousands of levels on a
# debug build), and building it allocates enough that several real
# collections run while the whole chain is live. Each is then walked
# iteratively to prove nothing was freed or corrupted.
class Node
  def initialize(next_node, label)
    @next_node = next_node
    @label = label
  end

  def next_node() = @next_node
  def label() = @label
end

DEPTH = 150000

array_chain = []
DEPTH.times() do |i|
  array_chain = [array_chain]
end
walked = 0
cursor = array_chain
while cursor.length() > 0
  cursor = cursor[0]
  walked += 1
end
puts("array chain: #{walked}")

hash_chain = {}
DEPTH.times() do |i|
  hash_chain = {"next": hash_chain}
end
walked = 0
cursor = hash_chain
while cursor.length() > 0
  cursor = cursor["next"]
  walked += 1
end
puts("hash chain: #{walked}")

list = nil
DEPTH.times() do |i|
  list = Node.new(list, "n#{i}")
end
walked = 0
cursor = list
while cursor != nil
  walked += 1
  cursor = cursor.next_node()
end
puts("linked instances: #{walked} (head #{list.label()})")

# A chain that alternates container kinds, so marking passes through all of
# them at every level.
mixed = nil
DEPTH.times() do |i|
  mixed = if i % 3 == 0
    [mixed]
  elsif i % 3 == 1
    {"next": mixed}
  else
    Node.new(mixed, "m")
  end
end
walked = 0
cursor = mixed
while cursor != nil
  walked += 1
  if cursor is Array
    cursor = cursor[0]
  elsif cursor is Hash
    cursor = cursor["next"]
  else
    cursor = cursor.next_node()
  end
end
puts("mixed chain: #{walked}")
