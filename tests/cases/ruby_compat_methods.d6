# Common Ruby methods on built-in values, implemented in the prelude
# (diamond_* functions reached through the extension protocol).
xs = [3, 1, 2]
ys = [1, 2, 3]
ys.insert(1, 9)
ys.unshift(0)
shifted = ys.shift()
deleted = ys.delete(9)
arrays = [
  xs.first(2), xs.last(2), xs.count(), xs.count(1), xs.count() do |x| x > 1 end,
  xs.size(), xs.find_index(2), xs.index(1), [1, 2, 3, 2].rindex(2),
  ys, shifted, deleted, xs.rotate(), xs.minmax(),
  [[1, 2], [3, 4]].to_h(), [[1, 2], [3, 4]].transpose(),
  xs.sum() do |x| x * 10 end, xs.filter_map() do |x| x * 2 if x > 1 end,
  xs.each_with_object([]) do |x, acc| acc.push(x) end,
  xs.none?() do |x| x > 5 end, xs.one?() do |x| x > 2 end,
  xs.inject(0) do |a, b| a + b end, [1, 2].product([3]), [1, 2, 3].combination(2),
  [1, 2, 4, 5, 7].chunk_while() do |a, b| b == a + 1 end,
  [1, 2, 3] - [2], [1, 2, 2, 3] & [2, 3, 4], [1, 2] | [2, 3], [1, 2] * 2, [1, 2] * "-",
  xs.values_at(0, 2), [[1, [2]]].dig(0, 1, 0), xs.shuffle().sort(), xs.include?(xs.sample()),
  [1, 2, 3][-1], [1, 2, 3, 4][1, 2]
]
h = {"b": 2, "a": 1}
hashes = [
  h.map() do |k, v| "#{k}=#{v}" end, h.map() do |v| v * 10 end,
  h.select() do |k, v| v > 1 end, h.select() do |v| v > 1 end,
  h.reject() do |k, v| v > 1 end, h.any?() do |k, v| k == "a" end,
  h.count(), h.count() do |k, v| v > 1 end, h.find() do |k, v| v == 2 end,
  h.min_by() do |k, v| v end, h.sort_by() do |k, v| v end, h.sort(),
  h.sum(), h.to_a(), h.key?("a"), h.value?(2), h.invert(),
  h.transform_values() do |v| v + 1 end, h.transform_keys() do |k| k + "!" end,
  h.slice("a"), h.except("a"), {"x": nil, "y": 1}.compact(),
  h.group_by() do |k, v| v % 2 end, h.partition() do |k, v| v > 1 end,
  h.fetch("a"), h.dig("a"), {"a": {"b": 3}}.dig("a", "b"), h.size()
]
missing = ""
begin
  h.fetch("zzz")
rescue e: IndexError
  missing = e.message()
end
strings = [
  "Hello".swapcase(), "abcb".index("b"), "abcb".rindex("b"), "hello".count("l"),
  "a\nb\n".lines(), "a  b c ".split(), "a,b,c".split(",", 2), "abc".center(7, "*"),
  "hello".delete("l"), "aaabbc".squeeze(), "ab" * 3, "ff".hex(), "abc".chop(),
  "abc".partition("b"), "Hello".casecmp?("hello"), "prefix-x".delete_prefix("prefix-"),
  "x.rb".delete_suffix(".rb"), "x".ljust(3) + "|", "abc".match?(Regexp.new("b")),
  "abc".to_sym(), "abcdef"[1, 3], "abc"[-1], "abc".size(), "ff".to_i(16)
]
numbers = [
  2 ** 10, -2 ** 2, 2 ** 3 ** 2, 2 ** 100, 2.0 ** 0.5 > 1.41, 2 ** -1,
  5.clamp(1, 3), 5.between?(1, 9), 7.divmod(2), 12.gcd(18), 4.lcm(6),
  1234.digits(), 4.even?(), 3.odd?(), 0.zero?(), 5.succ(), 5.pred(), 7.fdiv(2),
  255.to_s(16), (0 - 5).to_s(2), 5.5.truncate(), Math.sqrt(16), Math.hypot(3, 4),
  (1..10).step(3), (1..3).first(2)
]
[arrays, hashes, missing, strings, numbers]
