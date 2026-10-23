# Small typed building blocks. The generic ones work for any element type;
# the compiler checks each call against its annotations.

# Groups items by a key, keeping first-seen order of the keys.
# `[T, K]` are type parameters: T is the item type and K the key type, so
# the compiler can check that `key` really accepts a T and what the Hash holds.
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

# Average; an empty list gives 0.0 instead of dividing by zero.
def mean(values: Array[Int]) -> Float
  return 0.0 if values.empty?()
  to_f(values.sum()) / values.length()
end

# The middle value of the sorted list: the single middle one for an odd
# count, or the mean of the two middle ones for an even count.
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

# Letter grade for an average. A bare `case` tries each condition in order,
# so each test only needs the lower bound.
def letter(average: Float) -> String
  case
  when average >= 90.0 then "A"
  when average >= 80.0 then "B"
  when average >= 70.0 then "C"
  when average >= 60.0 then "D"
  else "F"
  end
end
