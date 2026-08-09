i = 0
result = nil
while i < 50
  re = Regexp.new("test\\d+")
  result = re.match?("test123")
  i = i + 1
end
result
