outer=loop do
 inner=loop do
  next if false
  break 20
 end
 break inner+22
end
outer
