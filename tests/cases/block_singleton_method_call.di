# A block on a singleton (class/module `self.`) method call, exercising
# parse_singleton_call's own trailing-DIAMOND_TOKEN_DO handling rather than
# parse_invoke's (instance methods) or parse_call's (direct calls).
class Runner
  def self.run(callback)
    callback()
  end

  def self.run_with(x, callback)
    callback(x)
  end
end

a = Runner.run() do
  1 + 1
end

b = Runner.run_with(5) do |n|
  n * n
end

puts(a)
puts(b)
