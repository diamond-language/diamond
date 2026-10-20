class Named
  def initialize(name)
    @name = name
  end

  def name() = @name

  def render(prefix) = prefix + @name
end

class LoudNamed < Named
  def decorate(prefix) = prefix + "> "

  def render(prefix) = self.decorate(prefix) + self.name()
end

LoudNamed.new("diamond").render("hello")
