module Outer
  interface Valuable
    def value() -> Int
  end

  class Reader
    def read(item: Valuable) -> Int
      item.value()
    end
  end
end
