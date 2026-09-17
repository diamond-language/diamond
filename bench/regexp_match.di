# Regexp#match throughput against reginold (docs/runtime-reference.md) --
# a real pattern with two capture groups, matched against a freshly built
# string each iteration so the loop isn't just re-matching one cached
# subject. No prior bench/*.di coverage existed for Regexp at all.
def run()
  pattern = Regexp.new("id:(\\d+)-(\\d+)")
  total = 0
  index = 0
  while index < 20000
    subject = "id:#{index}-#{index + 1}"
    m = pattern.match(subject)
    total = total + m[1].to_i()
    index = index + 1
  end
  total
end
run()
