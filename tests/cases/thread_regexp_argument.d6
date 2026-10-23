def check(pattern, text) = pattern.match?(text)

Thread.new(check, Regexp.new("b+"), "abbc").join()
