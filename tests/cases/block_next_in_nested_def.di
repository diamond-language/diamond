# A def nested inside a block is not the block: `next` there, outside a
# loop, is still an error.
[1].each() do |x|
  def inner(y)
    next if y
    1
  end
end
