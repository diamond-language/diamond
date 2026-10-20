class Parent
end

class Child < Parent
end

def choose(flag: Bool) -> Int | String
  if flag
    42
  else
    "diamond"
  end
end

def accept(value: Int | String | Parent) -> Int | String | Parent = value

accept(Child.new())
choose(false)
