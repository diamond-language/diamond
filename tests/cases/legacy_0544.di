def f()
 i = 0
 r = while i < 3
  i = i + 1
  break 99 if i == 2
 end
 r
end
"#{f()}, #{nil}"
