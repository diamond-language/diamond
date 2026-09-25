# self.private_method(...) do ... end is a call on self, like the same call
# without a block: plain, keyword, and spread arguments all reach it.
class Walker
  def initialize()
    @base = 10
  end
  def run()
    plain = self.step(1) do |x| x + @base end
    keyword = self.step(n: 2) do |x| x * @base end
    spread = self.step(*[3]) do |x| x - @base end
    [plain, keyword, spread]
  end
  private
  def step(n, &block) = yield(n)
end
Walker.new().run()
