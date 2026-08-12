module Outer
  module Middle
    module Values
      def answer()
        42
      end
    end
  end
end

class Box
  include Outer::Middle::Values
end

puts(Box.new().answer())
