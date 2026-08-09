re = Regexp.new("(\\d+)-(\\d+)")
m = re.match("id:42-99")
[m[0], m[1], m[2]]
