module Outer
  interface Base
    def value() -> Int
  end

  interface Child < Base
    def other() -> Int
  end
end

puts(42)
