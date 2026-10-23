# Array#each/#map/#select with a block -- exercises YIELD and the
# CALL_CLOSURE machinery a block invocation goes through, the
# overwhelmingly common way real Diamond code iterates (vs. the
# hand-written while loops every other benchmark in this suite uses).
# Kept small per outer iteration (a 100-element array) and repeated
# many times, rather than one huge array once, so DIAMOND_REPEAT's
# whole-chunk re-run methodology still applies cleanly.
def run()
  values = (1..100).to_a()
  total = 0
  index = 0
  while index < 4000
    values.each() do |value|
      total = total + value
    end
    doubled = values.map() do |value|
      value * 2
    end
    evens = values.select() do |value|
      value % 2 == 0
    end
    total = total + doubled.length() + evens.length()
    index = index + 1
  end
  total
end
run()
