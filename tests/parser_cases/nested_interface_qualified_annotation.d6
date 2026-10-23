module Outer
  interface Valuable
    def value() -> Int
  end

  class Box
    def value() -> Int
      42
    end
  end
end

def read(item: Outer::Valuable) -> Int
  item.value()
end

puts(read(Outer::Box.new()))
