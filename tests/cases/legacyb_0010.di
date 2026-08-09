class Greeting
  def initialize(name)
    @name = name
  end

  def render(prefix)
    prefix + @name
  end
end

greeting = Greeting.new("diamond")
greeting.render("hello, ")
