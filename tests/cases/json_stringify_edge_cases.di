def try(label, &block)
  begin
    r = yield
    puts("#{label}: #{r}")
  rescue e
    puts("#{label}: RAISED #{e.class()} #{e.message()}")
  end
end
class Foo
  def to_s() = "FOO"
end
try("scalars") do
  [JSON.stringify(nil), JSON.stringify(true), JSON.stringify(1), JSON.stringify(-5), JSON.stringify(1.5), JSON.stringify(100.0), JSON.stringify(1e20), JSON.stringify(1.0e-7)].join(" ")
end
try("big") do
  JSON.stringify(2 ** 100)
end
try("nan") do
  JSON.stringify(0.0 / 0.0)
end
try("inf") do
  JSON.stringify(1.0 / 0.0)
end
try("neginf") do
  JSON.stringify(-1.0 / 0.0)
end
try("escapes") do
  JSON.stringify("a\"b\\c\nd\re\tf\x01g\x1fh\x7fi/é€😀")
end
try("symbol key") do
  JSON.stringify({"a": 1})
end
try("str key") do
  JSON.stringify({"a": 1})
end
try("int key") do
  h = {}
  h[1] = 2
  h[2.5] = 3
  h[true] = 4
  h[nil] = 5
  JSON.stringify(h)
end
try("obj key") do
  h = {}
  h[Foo.new()] = 1
  JSON.stringify(h)
end
try("symbol value") do
  JSON.stringify("sym".to_sym())
end
try("instance") do
  JSON.stringify(Foo.new())
end
try("range") do
  JSON.stringify(1..3)
end
try("nested") do
  JSON.stringify([1, [2, {"a": [nil, true]}], "x"])
end
try("empty") do
  JSON.stringify([[], {}])
end
try("key escape") do
  JSON.stringify({"a\"b": 1})
end
try("frozen") do
  JSON.stringify([1].freeze())
end
