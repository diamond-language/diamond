# Small typed building blocks. The generic ones work for any element type;
# the compiler checks each call against its annotations.

# Groups items by a key, keeping first-seen order of the keys.
def group_by_key[T, K](items: Array[T], key: Callable[[T], K]) -> Hash[K, Array[T]]
  groups = {}
  items.each() do |item|
    k = key(item)
    groups[k] = groups.fetch(k, []).push(item)
  end
  groups
end

# The item with the largest `measure`, or nil for an empty Array.
def best_by[T](items: Array[T], measure: Callable[[T], Float]) -> T | Nil
  return nil if items.empty?()
  items.max_by() do |item| measure(item) end
end

def mean(values: Array[Int]) -> Float
  return 0.0 if values.empty?()
  to_f(values.sum()) / values.length()
end

def median(values: Array[Int]) -> Float
  return 0.0 if values.empty?()
  sorted = values.sort()
  middle = sorted.length() / 2
  if sorted.length() % 2 == 1
    to_f(sorted[middle])
  else
    to_f(sorted[middle - 1] + sorted[middle]) / 2
  end
end

def letter(average: Float) -> String
  case
  when average >= 90.0 then "A"
  when average >= 80.0 then "B"
  when average >= 70.0 then "C"
  when average >= 60.0 then "D"
  else "F"
  end
end
