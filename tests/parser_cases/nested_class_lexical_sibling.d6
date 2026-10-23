module Outer
  class Box
    def answer()
      42
    end
  end

  class Maker
    def make()
      Box.new()
    end
  end
end

puts(Outer::Maker.new().make().answer())
