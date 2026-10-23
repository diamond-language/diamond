module Outer
  module Values
    def answer()
      42
    end
  end
end

class Box
  include Outer::Values
end

puts(Box.new().answer())
