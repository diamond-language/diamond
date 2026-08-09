cleanup=0
value=begin
 [1][4]
rescue : TypeError
 0
rescue : IndexError
 40
ensure
 cleanup=cleanup+1
end
[value+2, cleanup]
