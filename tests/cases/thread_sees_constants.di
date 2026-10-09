LIMIT = 30
NAME = "page"
SIZES = [10, 20]
LOOKUP = {"a": 1}
PATTERN = Regexp.new("^[a-z]+$")
TOTAL = LIMIT * 2 + SIZES.length()

def describe()
  [LIMIT, NAME, SIZES, LOOKUP, PATTERN.match?("abc"), PATTERN.match?("ABC"), TOTAL].inspect()
end

Thread.new(describe).join()
