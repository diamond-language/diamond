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
  delegate labeled(label, *arguments), to: @target
end

class Target
  def labeled(label, *arguments)
    "#{label}:#{arguments.join("-")}"
  end
end

wrapper = Wrapper.new(Target.new())
puts(wrapper.render())
puts(wrapper.render("alpha", "beta", "gamma"))
puts(wrapper.labeled("items", "one", "two"))
nil
