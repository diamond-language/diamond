data = {"b": [1, 2, {"z": nil, "a": true}], "a": {}, "c": [], 3: "three", "d": {"y": 1.5, "x": "q\"uote"}}
raised = []
[{"indent": -1}, {"indent": 99}, {"indent": " x"}, {"indent": 2.5}, {"sort_keys": 1}, {"pretty": true}, 5].each() do |bad|
  begin
    JSON.stringify(data, bad)
  rescue error: TypeError
    raised.push(error.message())
  end
end
[
  JSON.stringify(data),
  JSON.stringify(data, {"sort_keys": true}),
  JSON.stringify(data, {"indent": 2}),
  JSON.stringify(data, {"indent": 2, "sort_keys": true}),
  JSON.stringify(data, {"indent": "\t"}),
  JSON.stringify([], {"indent": 2}),
  JSON.stringify({}, {"indent": 2}),
  JSON.stringify([1, [2, [3]]], {"indent": 1}),
  JSON.stringify("text", {"indent": 2}),
  JSON.stringify(data, {"indent": 0}) == JSON.stringify(data),
  JSON.stringify(data, nil) == JSON.stringify(data),
  JSON.parse(JSON.stringify(data, {"indent": 4, "sort_keys": true})) == JSON.parse(JSON.stringify(data)),
  raised,
]
