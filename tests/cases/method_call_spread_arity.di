class Receiver
  def call(left, right) = left + right
end

Receiver.new().call(*[1])
