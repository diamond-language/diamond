class Box
  def initialize()
    def make_cb() = 42
    @cb = make_cb
  end
  def run() = @cb()
end
Box.new().run()
