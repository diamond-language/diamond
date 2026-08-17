class C
  def self.run(v)
    def doubler(x)
      x * 2
    end
    doubler(v)
  end

  def self.make_adder(n)
    def add(x)
      x + n
    end
    add
  end
end

adder = C.make_adder(5)
[C.run(21), adder(10)]
