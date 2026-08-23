require "../../packages/rack/lib/rack"

def build_first()
  "first"
end

def build_second()
  "second"
end

first = RackChain.get(build_first)
second = RackChain.get(build_second)
third = RackChain.get(build_first)

[first, second, third]
