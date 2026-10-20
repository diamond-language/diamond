require "../../packages/rack/lib/rack"

def build_first() = "first"

def build_second() = "second"

first = RackChain.get(build_first)
second = RackChain.get(build_second)
third = RackChain.get(build_first)

[first, second, third]
