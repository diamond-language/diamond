# A non-String Hash key is converted with its own to_s, which is arbitrary
# code and may change the very Hash being encoded. Encoding must stay
# memory-safe and finish.
class Shrinker
  def initialize(target)
    @target = target
  end

  def to_s()
    @target.delete("later")
    "shrinker"
  end
end

h = {}
shrinker = Shrinker.new(h)
h[shrinker] = 1
h["later"] = 2
puts(JSON.stringify(h))

# A to_s that deletes the key being converted.
class SelfRemover
  def initialize(target)
    @target = target
  end

  def to_s()
    @target.delete(self)
    "gone"
  end
end

g = {}
g[SelfRemover.new(g)] = 1
g["x"] = 2
puts(JSON.stringify(g).length() > 0)
