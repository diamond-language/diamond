re = Regexp.new("(a)|(b)")
m = re.match("b")
[m[0], m[1], m[2]]
