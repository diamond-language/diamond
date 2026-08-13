def main()
  def is_even(x: Int) -> Bool
    mod(x, 2) == 0
  end

  def gt2(x: Int) -> Bool
    x > 2
  end

  def negate(x: Int) -> Int
    -x
  end

  def report_pair(value: Int, index: Int)
    puts("#{index}=#{value}")
  end

  puts([1, 2, 3, 4].sum())

  puts([3, 1, 4, 1, 5].sort())
  puts([3, 1, 4, 1, 5].sort_by(negate))

  puts([1, 2, 3, 4, 5].reject(is_even))

  puts([1, 2, 3, 4].find(gt2))

  [10, 20, 30].each_with_index(report_pair)

  puts([5, 3, 8, 1].min())
  puts([5, 3, 8, 1].max())
end

main()
