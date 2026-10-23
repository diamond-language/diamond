module Outer
  module Values
    def answer()
      42
    end
  end

  module Combined
    include Values
  end
end

class Box
  include Outer::Combined
end

puts(Box.new().answer())
