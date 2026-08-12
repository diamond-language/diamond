module Outer
  module Middle
    class Box
      def answer()
        42
      end
    end
  end
end

puts(Outer::Middle::Box.new().answer())
