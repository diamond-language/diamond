def triple(first, second, third)
  [first, second, third]
end

callable = triple
callable(*[1], third: 3)
