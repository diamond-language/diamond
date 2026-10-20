class C
  def initialize()
    def doubler(x) = x * 2
    @@fn = doubler
  end
  def self.run(v) = @@fn(v)
end
C.new()
C.run(21)
