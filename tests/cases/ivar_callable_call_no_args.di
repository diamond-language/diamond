class Box
  def initialize()
    def make_cb()
      42
    end
    @cb = make_cb
  end
  def run()
    @cb()
  end
end
Box.new().run()
