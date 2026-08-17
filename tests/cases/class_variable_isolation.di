class A
  def self.set(v)
    @@x = v
  end
  def self.get()
    @@x
  end
end
class B
  def self.set(v)
    @@x = v
  end
  def self.get()
    @@x
  end
end
A.set("from A")
B.set("from B")
"#{A.get()}, #{B.get()}"
