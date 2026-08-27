def receive(*values, &block)
  puts(values)
  puts(block == nil)
  if block != nil
    puts(block(values.length))
  end
end

def forward(*values, &block)
  receive(*values, &block)
end

class ForwardTarget
  def receive(*values, &block)
    puts(values)
    puts(block == nil)
    if block != nil
      puts(block(values.length))
    end
  end
end

def forward_method(target, *values, &block)
  target.receive(*values, &block)
end

def forward_callable(callback: Callable, *values, &block)
  callback(*values, &block)
end

def callable_receive(*values, &block)
  puts(values)
  puts(block == nil)
  if block != nil
    puts(block(values.length))
  end
end

forward(1, 2)
forward(3, 4) do |count|
  "function-block"
end

target = ForwardTarget.new()
forward_method(target, 5, 6)
forward_method(target, 7) do |count|
  "method-block"
end

receiver = callable_receive
forward_callable(receiver, 8, 9)
forward_callable(receiver, 10) do |count|
  "callable-block"
end
