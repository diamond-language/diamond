def check(pattern, text)
  pattern.match?(text)
end

Thread.new(check, Regexp.new("b+"), "abbc").join()
