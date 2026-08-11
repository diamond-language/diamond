module Outer
  module Values
    ANSWER = 42
  end
end

puts(Outer::Values::ANSWER)
