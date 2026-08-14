re = Regexp.new("(x)?a")
"abc".gsub(re, "[\\1]")
