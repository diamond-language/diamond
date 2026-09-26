# `break` in a do-block ends the call the block was passed to; `return`
# returns from the enclosing def. Both run ensure clauses on the way out
# and are never caught by rescue.
log = []

def first_over(values: Array[Int], limit: Int) -> Int | Nil
  values.each() do |v|
    return v if v > limit
  end
  nil
end

def pair_sum(target: Int) -> String
  [1, 2, 3].each() do |a|
    [4, 5, 6].each() do |b|
      return "#{a}+#{b}" if a + b == target
    end
  end
  "none"
end

def guarded(log: Array[String]) -> Int
  begin
    [10, 20, 30].each() do |v|
      begin
        return v if v == 20
      ensure
        log.push("inner #{v}")
      end
    end
  rescue e: Exception
    log.push("rescued")
  ensure
    log.push("outer")
  end
  0
end

def fact(n: Int) -> Int
  return 1 if n <= 1
  [n].each() do |m|
    return m * fact(m - 1)
  end
  0
end

found = [3, 8, 12].each() do |v|
  break v if v > 5
end
bare = [1, 2].each() do |v|
  break
end
mapped = [1, 2, 3].map() do |x|
  break "stopped at #{x}" if x == 2
  x
end
sum = 0
[1, 2, 3].each() do |x|
  [10, 20, 30].each() do |y|
    break if y == 20
    sum += x * y
  end
end
counted = 0
[1, 2].each() do |x|
  i = 0
  while true
    i += 1
    break if i == 3
  end
  counted += i
end
times = 5.times() do |i|
  break i * 7 if i == 3
end
sorted = [3, 1, 2].sort_by() do |x|
  break [] if x == 2
  x
end
g = guarded(log)
[first_over([1, 5, 9], 4), first_over([1], 4), pair_sum(8), g, log, fact(6),
 found, bare, mapped, sum, counted, times, sorted]
