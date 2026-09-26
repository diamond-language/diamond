# JSON.stringify escapes what JSON requires, keeps insertion order, and
# rejects values JSON can't represent.
text = JSON.stringify({"b": [1, 2.5, nil, true], "a": "quote\" slash\\ tab\t", 3: "three"})
failures = [0.0 / 0.0, 1.0 / 0.0, :symbol].map() do |value|
  begin
    JSON.stringify(value)
  rescue error: JSONError
    "rejected"
  end
end
[text, JSON.parse(text)["a"] == "quote\" slash\\ tab\t", failures]
