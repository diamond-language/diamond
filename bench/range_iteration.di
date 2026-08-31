# Range#each -- a distinct code path from both a hand-written while
# loop (int_arithmetic.di) and an Array#each over a materialized
# Array (iterator_blocks.di): no backing Array ever gets allocated,
# each value is produced by the Range itself as it's iterated.
def run()
  total = 0
  outer = 0
  while outer < 5000
    (1..200).each() do |value|
      total = total + value
    end
    outer = outer + 1
  end
  total
end
run()
