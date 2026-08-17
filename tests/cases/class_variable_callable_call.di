class C
  def initialize()
    def doubler(x)
      x * 2
    end
    @@fn = doubler
  end
  def self.run(v)
    @@fn(v)
  end
end
C.new()
C.run(21)
