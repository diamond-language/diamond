class Widget
  def initialize(x)
    @x = x
  end

  def show()
    self.helper()
  end

  private

  def helper()
    @x
  end
end
w = Widget.new(42)
puts(w.show())
begin
  w.helper()
rescue
  puts("blocked")
end
