class Box
  def make_adder(n)
    def add(x)
      x + n
    end
    add
  end
end

class Counter
  def make(value)
    base = value
    def level1()
      def level2()
        base = base + 1
      end
      level2
    end
    level1
  end
end

adder = Box.new().make_adder(5)
l1 = Counter.new().make(10)
l2 = l1()
[adder(10), l2(), l2()]
