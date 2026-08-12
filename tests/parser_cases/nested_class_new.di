module Outer
  class Box
    def answer()
      42
    end
  end
end

puts(Outer::Box.new().answer())
