class Formatter
  def join3(left, middle, right)
    left + ":" + middle + ":" + right
  end

  def total(prefix, *values)
    sum = 0
    values.each() do |value|
      sum += value
    end
    "#{prefix}#{sum}"
  end
end

class ChildFormatter < Formatter
end

class Dynamic
  def method_missing(name, arguments)
    "#{name}:#{arguments.join(",")}"
  end
end

formatter = ChildFormatter.new()
puts(formatter.join3(*["a", "b", "c"]))
puts(formatter.total(*["sum=", 1, 2, 3, 4]))
puts(Dynamic.new().unknown(*["x", "y"]))

empty = []
class Empty
  def answer() = 42
end
puts(Empty.new().answer(*empty))

class Vault
  def reveal(arguments)
    self.secret(*arguments)
  end

  private

  def secret(value)
    "secret=#{value}"
  end
end
puts(Vault.new().reveal([9]))

class Peer
  def forward(other, arguments)
    other.protected_join(*arguments)
  end

  protected

  def protected_join(left, right)
    left + right
  end
end
puts(Peer.new().forward(Peer.new(), ["peer", "-ok"]))
nil
