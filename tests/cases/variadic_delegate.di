class Target
  def render(*parts)
    parts.join("-")
  end
end

class Wrapper
  def initialize(target)
    @target = target
  end

  delegate render(*arguments), to: @target
end

wrapper = Wrapper.new(Target.new())
puts(wrapper.render())
puts(wrapper.render("alpha", "beta", "gamma"))
nil
