# A top-level function of the same name still wins over a sibling singleton
# method, as it does for instance methods.
def pick() = "function"
class Chooser
  def self.go() = pick()
  def self.pick() = "singleton"
end
puts(Chooser.go())
puts(Chooser.pick())
