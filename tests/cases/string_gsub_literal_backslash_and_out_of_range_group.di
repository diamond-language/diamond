re = Regexp.new("a")
[
  "a".gsub(re, "\\\\n"),
  "a".gsub(re, "\\9"),
]
