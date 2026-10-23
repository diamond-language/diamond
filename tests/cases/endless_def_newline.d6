# The body of an endless def may start on the line after the `=`.
def add_one(x) =
  x + 1

def first_positive(x) =

  # a comment between
  x if x > 0

def trailing(a) = # comment after the equals
  a * 2

class Box
  def initialize(n)
    @n = n
  end

  def doubled() =
    @n * 2

  def self.make(n) =
    Box.new(n)
end

module Util
  def self.greet(name) =
    "hi " + name + "!"
end

[add_one(1), first_positive(4), first_positive(-4), trailing(5), Box.make(21).doubled(), Util.greet("ada")]
