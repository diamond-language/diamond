# to_s() works on every built-in value and gives the same text string
# interpolation does.
values = [1, 1.5, true, nil, "s", :sym, [1, "a"], {"k": [2]}, 0...3]
values.map() do |value| value.to_s() == "#{value}" end + [values.map() do |value| value.to_s() end]
