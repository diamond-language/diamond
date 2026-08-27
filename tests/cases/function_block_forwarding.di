def apply_four(first, second, third, callback: Callable[1])
  callback(first + second + third)
end

def forward_apply(first, *remaining, &block)
  apply_four(first, *remaining, &block)
end

class MethodBlockTarget
  def apply_four(first, second, third, callback: Callable[1])
    callback(first * second * third)
  end
end

def forward_method(target, *arguments, &block)
  target.apply_four(*arguments, &block)
end

puts(forward_apply(2, 3, 4) do |sum|
  sum * 5
end)

puts(forward_method(MethodBlockTarget.new(), 2, 3, 4) do |product|
  product + 1
end)
