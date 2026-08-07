class BaseMessage
  def initialize(name)
    @name = name
  end

  def render(prefix)
    prefix + @name
  end
end

class DecoratedMessage < BaseMessage
  def initialize(name)
    super(name + "!")
  end

  def render(prefix)
    super(prefix + "> ")
  end
end

DecoratedMessage.new("diamond").render("hello")
