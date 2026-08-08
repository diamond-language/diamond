# String concatenation and the round-2/3 native String primitives in a
# loop -- concatenation exercises ADD's unquickened String branch
# (there is no unquickened ADD deopt cost here since operands are
# String every time, but ADD is never specialized to ADD_INT either);
# the rest exercise DIAMOND_OP_INVOKE's String dispatch block.
def run()
  index = 0
  total = 0
  while index < 200000
    text = "hello, " + "world"
    upper = text.upcase()
    reversed = upper.reverse()
    pieces = text.split(", ")
    sliced = text.slice(0, 5)
    total = total + sliced.length() + reversed.length() + pieces.length()
    index = index + 1
  end
  total
end
run()
