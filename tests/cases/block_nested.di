# A block whose own body contains another block-taking call -- exercises
# compile_block's outer-state save/restore nesting two levels deep.
result = [1, 2, 3].map() do |x|
  [10, 20].reduce(0) do |acc, y|
    acc + x * y
  end
end
result
