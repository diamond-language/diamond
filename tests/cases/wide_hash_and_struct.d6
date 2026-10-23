require "../../lib/minitest"

# Forty entries: well past the old sixteen-entry literal cap.
WIDE = {
  "k0": 0, "k1": 1, "k2": 2, "k3": 3, "k4": 4, "k5": 5, "k6": 6, "k7": 7,
  "k8": 8, "k9": 9, "k10": 10, "k11": 11, "k12": 12, "k13": 13, "k14": 14,
  "k15": 15, "k16": 16, "k17": 17, "k18": 18, "k19": 19, "k20": 20,
  "k21": 21, "k22": 22, "k23": 23, "k24": 24, "k25": 25, "k26": 26,
  "k27": 27, "k28": 28, "k29": 29, "k30": 30, "k31": 31, "k32": 32,
  "k33": 33, "k34": 34, "k35": 35, "k36": 36, "k37": 37, "k38": 38,
  "k39": 39
}

struct Account(
  id: Int,
  name: String,
  email: String,
)
end

struct Point(x: Int,
             y: Int)
end

def run_tests()
  suite = Minitest.new()

  suite.test("a hash literal may have more than sixteen entries") do
    Minitest.assert_equal(40, WIDE.length())
    Minitest.assert_equal(0, WIDE["k0"])
    Minitest.assert_equal(16, WIDE["k16"])
    Minitest.assert_equal(39, WIDE["k39"])
  end

  suite.test("a struct field list may span lines, with a trailing comma") do
    account = Account.new(1, "Ada", "ada@example.com")
    Minitest.assert_equal("Ada", account.name())
    Minitest.assert_equal("ada@example.com", account.email())
  end

  suite.test("a struct field list may break after any comma") do
    point = Point.new(3, 4)
    Minitest.assert_equal(7, point.x() + point.y())
  end

  suite.run!()
end

run_tests()
