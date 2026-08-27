def collect(&block: Callable[[], Array[Int]]) -> Array[Int]
  yield()
end

def generic_collect[T](value: T, &block: Callable[[], Array[T]]) -> Array[T]
  yield()
end

empty = collect() do
  []
end
empty << 40
puts(empty[0] + 2)

generic = generic_collect(21) do
  []
end
generic << 21
puts(generic[0] * 2)
