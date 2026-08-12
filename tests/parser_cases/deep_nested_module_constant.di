module Outer
  module Middle
    module Inner
      ANSWER = 42
    end
  end
end

puts(Outer::Middle::Inner::ANSWER)
