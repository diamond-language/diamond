class Named
  def initialize(name)
    @name = name
  end

  def name()
    @name
  end

  def render(prefix)
    prefix + @name
  end
end

class LoudNamed < Named
  def decorate(prefix)
    prefix + "> "
  end

  def render(prefix)
    self.decorate(prefix) + self.name()
  end
end

LoudNamed.new("diamond").render("hello")
