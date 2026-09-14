struct Node(value: Int, rest: Node | Nil)
end

tail = Node.new(2, nil)
head = Node.new(1, tail)
puts(head.value())
puts(head.rest().value())
