class Walker
  def poke(other) = other.step(1) do |x| x end
  private
  def step(n, &block) = yield(n)
end
Walker.new().poke(Walker.new())
