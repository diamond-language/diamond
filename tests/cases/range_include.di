a = (1..5).include?(5)
b = (1...5).include?(5)
c = (1..5).include?(0)
d = (1..5).include?(1)
"#{a}, #{b}, #{c}, #{d}"
