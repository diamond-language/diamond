# A plain def nested in a method has no self, so a bare call there is
# still an ordinary function call.
class A
  def helper(x: Int) -> Int = x + 1
  def run() -> Int
    def inner(v: Int) -> Int
      helper(v)
    end
    inner(5)
  end
end
