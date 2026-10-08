require "../../lib/minitest"

def run_tests()
  suite = Minitest.new()

  suite.test("a method chain continues on lines starting with a dot") do
    result = [3, 1, 2]
      .sort()
      .map() do |value| value * 2 end
      .reverse()
    Minitest.assert_equal([6, 4, 2], result)
  end

  suite.test("comments and blank lines may sit between chained lines") do
    result = "  Ada  "
      .strip()   # trailing comment

      # a comment line between links
      .upcase()
    Minitest.assert_equal("ADA", result)
  end

  suite.test("a leading dot still works after a plain expression statement") do
    text = "a-b"
    parts = text
      .split("-")
    Minitest.assert_equal(["a", "b"], parts)
  end

  suite.test("ranges at the start of a line are not method calls") do
    total = 0
    (1..3).each() do |value| total += value end
    Minitest.assert_equal(6, total)
  end

  suite.test("statements without a leading dot stay separate") do
    first = 1
    second = 2
    Minitest.assert_equal(3, first + second)
  end

  suite.run!()
end

run_tests()
