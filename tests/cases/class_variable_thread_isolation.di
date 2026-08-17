class Shared
  def self.set(v)
    @@x = v
  end
  def self.get()
    @@x
  end
end
def worker()
  Shared.set("child")
  Shared.get()
end
Shared.set("parent")
t = Thread.new(worker)
child_result = t.join()
"#{Shared.get()}, #{child_result}"
