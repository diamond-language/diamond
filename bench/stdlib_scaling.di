def ints(n)
  seed = 12345
  a = []
  n.times() do |i|
    seed = (seed * 1103515245 + 12345) % 2147483648
    a.push(seed % 100000)
  end
  a
end

def hash_of(n)
  h = {}
  n.times() do |i|
    h[i] = i * 2
  end
  h
end

def text(n)
  words = ["alpha", "beta", "gamma", "delta", "epsilon"]
  s = StringBuilder.new()
  i = 0
  while s.length() < n
    s.append(words[i % 5])
    s.append(" ")
    i += 1
  end
  s.to_s()
end

def best_of(trials, base, work: Callable[1])
  best = nil
  trials.times() do |t|
    elapsed = work(base)
    best = elapsed if best == nil || elapsed < best
  end
  best
end

def measure(label, base, work: Callable[1])
  small = best_of(3, base, work)
  large = best_of(3, base * 4, work)
  ratio = small > 0.0 ? large / small : 0.0
  puts("#{label}|#{base}|#{(small * 1000.0).round(2)}|#{(large * 1000.0).round(2)}|#{ratio.round(1)}")
end

begin
  measure("Array#first(n)", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.first(n / 2)
    Time.monotonic() - start
  end
rescue e
  puts("Array#first(n)|ERROR|#{e.message()}")
end

begin
  measure("Array#last(n)", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.last(n / 2)
    Time.monotonic() - start
  end
rescue e
  puts("Array#last(n)|ERROR|#{e.message()}")
end

begin
  measure("Array#include?", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.include?(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#include?|ERROR|#{e.message()}")
end

begin
  measure("Array#index_of", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.index_of(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#index_of|ERROR|#{e.message()}")
end

begin
  measure("Array#each", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.each() do |x|
      x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#each|ERROR|#{e.message()}")
end

begin
  measure("Array#map", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.map() do |x|
      x + 1
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#map|ERROR|#{e.message()}")
end

begin
  measure("Array#reverse", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.reverse()
    Time.monotonic() - start
  end
rescue e
  puts("Array#reverse|ERROR|#{e.message()}")
end

begin
  measure("Array#concat", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.concat(a)
    Time.monotonic() - start
  end
rescue e
  puts("Array#concat|ERROR|#{e.message()}")
end

begin
  measure("Array#compact", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.compact()
    Time.monotonic() - start
  end
rescue e
  puts("Array#compact|ERROR|#{e.message()}")
end

begin
  measure("Array#uniq", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.uniq()
    Time.monotonic() - start
  end
rescue e
  puts("Array#uniq|ERROR|#{e.message()}")
end

begin
  measure("Array#flatten", 20000) do |n|
    a = ints(n)
    nested = a.each_slice(4).map() do |s|
      s
    end
    start = Time.monotonic()
    nested.flatten()
    Time.monotonic() - start
  end
rescue e
  puts("Array#flatten|ERROR|#{e.message()}")
end

begin
  measure("Array#delete_at(0)", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.delete_at(0)
    Time.monotonic() - start
  end
rescue e
  puts("Array#delete_at(0)|ERROR|#{e.message()}")
end

begin
  measure("Array#sum", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.sum()
    Time.monotonic() - start
  end
rescue e
  puts("Array#sum|ERROR|#{e.message()}")
end

begin
  measure("Array#reject", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.reject() do |x|
      x % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#reject|ERROR|#{e.message()}")
end

begin
  measure("Array#find", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.find() do |x|
      x < 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#find|ERROR|#{e.message()}")
end

begin
  measure("Array#each_with_index", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.each_with_index() do |x, i|
      x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#each_with_index|ERROR|#{e.message()}")
end

begin
  measure("Array#sort", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.sort()
    Time.monotonic() - start
  end
rescue e
  puts("Array#sort|ERROR|#{e.message()}")
end

begin
  measure("Array#sort_by", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.sort_by() do |x|
      -x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#sort_by|ERROR|#{e.message()}")
end

begin
  measure("Array#min", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.min()
    Time.monotonic() - start
  end
rescue e
  puts("Array#min|ERROR|#{e.message()}")
end

begin
  measure("Array#max", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.max()
    Time.monotonic() - start
  end
rescue e
  puts("Array#max|ERROR|#{e.message()}")
end

begin
  measure("Array#min_by", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.min_by() do |x|
      -x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#min_by|ERROR|#{e.message()}")
end

begin
  measure("Array#take", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.take(n / 2)
    Time.monotonic() - start
  end
rescue e
  puts("Array#take|ERROR|#{e.message()}")
end

begin
  measure("Array#drop", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.drop(1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#drop|ERROR|#{e.message()}")
end

begin
  measure("Array#flat_map", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.flat_map() do |x|
      [x, x]
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#flat_map|ERROR|#{e.message()}")
end

begin
  measure("Array#partition", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.partition() do |x|
      x % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#partition|ERROR|#{e.message()}")
end

begin
  measure("Array#group_by", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.group_by() do |x|
      x % 10
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#group_by|ERROR|#{e.message()}")
end

begin
  measure("Array#zip", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.zip(a)
    Time.monotonic() - start
  end
rescue e
  puts("Array#zip|ERROR|#{e.message()}")
end



begin
  measure("Array#tally", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.tally()
    Time.monotonic() - start
  end
rescue e
  puts("Array#tally|ERROR|#{e.message()}")
end

begin
  measure("Array#insert(0)", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.insert(0, 1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#insert(0)|ERROR|#{e.message()}")
end

begin
  measure("Array#unshift", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.unshift(1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#unshift|ERROR|#{e.message()}")
end

begin
  measure("Array#shift", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.shift()
    Time.monotonic() - start
  end
rescue e
  puts("Array#shift|ERROR|#{e.message()}")
end

begin
  measure("Array#count", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.count()
    Time.monotonic() - start
  end
rescue e
  puts("Array#count|ERROR|#{e.message()}")
end

begin
  measure("Array#find_index", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.find_index(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#find_index|ERROR|#{e.message()}")
end

begin
  measure("Array#rindex", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.rindex(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#rindex|ERROR|#{e.message()}")
end

begin
  measure("Array#delete", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.delete(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#delete|ERROR|#{e.message()}")
end

begin
  measure("Array#delete_if", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.delete_if() do |x|
      x % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#delete_if|ERROR|#{e.message()}")
end

begin
  measure("Array#keep_if", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.keep_if() do |x|
      x % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#keep_if|ERROR|#{e.message()}")
end

begin
  measure("Array#fill", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.fill(0)
    Time.monotonic() - start
  end
rescue e
  puts("Array#fill|ERROR|#{e.message()}")
end

begin
  measure("Array#values_at", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.values_at(0, 1, 2)
    Time.monotonic() - start
  end
rescue e
  puts("Array#values_at|ERROR|#{e.message()}")
end

begin
  measure("Array#to_h", 20000) do |n|
    a = ints(n)
    pairs = a.map() do |x|
      [x, x]
    end
    start = Time.monotonic()
    pairs.to_h()
    Time.monotonic() - start
  end
rescue e
  puts("Array#to_h|ERROR|#{e.message()}")
end

begin
  measure("Array#transpose", 20000) do |n|
    a = ints(n)
    pairs = a.map() do |x|
      [x, x]
    end
    start = Time.monotonic()
    pairs.transpose()
    Time.monotonic() - start
  end
rescue e
  puts("Array#transpose|ERROR|#{e.message()}")
end

begin
  measure("Array#minmax", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.minmax()
    Time.monotonic() - start
  end
rescue e
  puts("Array#minmax|ERROR|#{e.message()}")
end

begin
  measure("Array#filter_map", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.filter_map() do |x|
      x if x % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#filter_map|ERROR|#{e.message()}")
end

begin
  measure("Array#each_with_object", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.each_with_object([]) do |x, acc|
      acc.push(x)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#each_with_object|ERROR|#{e.message()}")
end

begin
  measure("Array#none?", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.none?() do |x|
      x < 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#none?|ERROR|#{e.message()}")
end

begin
  measure("Array#inject", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.inject(0) do |acc, x|
      acc + x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#inject|ERROR|#{e.message()}")
end

begin
  measure("Array#reduce", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.reduce(0) do |acc, x|
      acc + x
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#reduce|ERROR|#{e.message()}")
end

begin
  measure("Array#chunk_while", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.chunk_while() do |x, y|
      x < y
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#chunk_while|ERROR|#{e.message()}")
end

begin
  measure("Array#rotate", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.rotate(1)
    Time.monotonic() - start
  end
rescue e
  puts("Array#rotate|ERROR|#{e.message()}")
end

begin
  measure("Array#-", 500) do |n|
    a = ints(n)
    start = Time.monotonic()
    a - a
    Time.monotonic() - start
  end
rescue e
  puts("Array#-|ERROR|#{e.message()}")
end

begin
  measure("Array#&", 500) do |n|
    a = ints(n)
    start = Time.monotonic()
    a & a
    Time.monotonic() - start
  end
rescue e
  puts("Array#&|ERROR|#{e.message()}")
end

begin
  measure("Array#|", 500) do |n|
    a = ints(n)
    start = Time.monotonic()
    a | a
    Time.monotonic() - start
  end
rescue e
  puts("Array#||ERROR|#{e.message()}")
end

begin
  measure("Array#*", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a * 2
    Time.monotonic() - start
  end
rescue e
  puts("Array#*|ERROR|#{e.message()}")
end

begin
  measure("Array#join", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.join(",")
    Time.monotonic() - start
  end
rescue e
  puts("Array#join|ERROR|#{e.message()}")
end

begin
  measure("Array#combination(2)", 300) do |n|
    a = ints(n)
    start = Time.monotonic()
    a.combination(2)
    Time.monotonic() - start
  end
rescue e
  puts("Array#combination(2)|ERROR|#{e.message()}")
end


begin
  measure("Array#push loop", 20000) do |n|
    a = ints(n)
    start = Time.monotonic()
    b = []
    a.each() do |x|
      b.push(x)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#push loop|ERROR|#{e.message()}")
end

begin
  measure("Array#+ loop", 4000) do |n|
    a = ints(n)
    start = Time.monotonic()
    b = []
    (n / 20).times() do |i|
      b = b + [i]
    end
    Time.monotonic() - start
  end
rescue e
  puts("Array#+ loop|ERROR|#{e.message()}")
end

begin
  measure("Hash#include_key?", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.include_key?(-1)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#include_key?|ERROR|#{e.message()}")
end

begin
  measure("Hash#fetch", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.fetch(1, 0)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#fetch|ERROR|#{e.message()}")
end

begin
  measure("Hash#each", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.each() do |k, v|
      v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#each|ERROR|#{e.message()}")
end

begin
  measure("Hash#keys", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.keys()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#keys|ERROR|#{e.message()}")
end

begin
  measure("Hash#values", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.values()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#values|ERROR|#{e.message()}")
end

begin
  measure("Hash#map_values", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.map_values() do |v|
      v + 1
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#map_values|ERROR|#{e.message()}")
end

begin
  measure("Hash#merge", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.merge(h)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#merge|ERROR|#{e.message()}")
end

begin
  measure("Hash#select", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.select() do |k, v|
      v % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#select|ERROR|#{e.message()}")
end

begin
  measure("Hash#count", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.count() do |k, v|
      v % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#count|ERROR|#{e.message()}")
end

begin
  measure("Hash#any?", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.any?() do |k, v|
      v < 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#any?|ERROR|#{e.message()}")
end

begin
  measure("Hash#all?", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.all?() do |k, v|
      v >= 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#all?|ERROR|#{e.message()}")
end

begin
  measure("Hash#map", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.map() do |k, v|
      v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#map|ERROR|#{e.message()}")
end

begin
  measure("Hash#reduce", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.reduce(0) do |acc, pair|
      acc + 1
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#reduce|ERROR|#{e.message()}")
end

begin
  measure("Hash#to_a", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.to_a()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#to_a|ERROR|#{e.message()}")
end

begin
  measure("Hash#sort_by", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.sort_by() do |k, v|
      -v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#sort_by|ERROR|#{e.message()}")
end

begin
  measure("Hash#min_by", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.min_by() do |k, v|
      -v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#min_by|ERROR|#{e.message()}")
end

begin
  measure("Hash#group_by", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.group_by() do |k, v|
      v % 10
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#group_by|ERROR|#{e.message()}")
end

begin
  measure("Hash#transform_values", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.transform_values() do |v|
      v + 1
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#transform_values|ERROR|#{e.message()}")
end

begin
  measure("Hash#transform_keys", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.transform_keys() do |k|
      k
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#transform_keys|ERROR|#{e.message()}")
end

begin
  measure("Hash#invert", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.invert()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#invert|ERROR|#{e.message()}")
end

begin
  measure("Hash#update", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.update(h)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#update|ERROR|#{e.message()}")
end

begin
  measure("Hash#slice", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.slice(1, 2, 3)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#slice|ERROR|#{e.message()}")
end

begin
  measure("Hash#except", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.except(1, 2, 3)
    Time.monotonic() - start
  end
rescue e
  puts("Hash#except|ERROR|#{e.message()}")
end

begin
  measure("Hash#compact", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.compact()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#compact|ERROR|#{e.message()}")
end

begin
  measure("Hash#each_with_index", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.each_with_index() do |pair, i|
      i
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#each_with_index|ERROR|#{e.message()}")
end

begin
  measure("Hash#filter_map", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.filter_map() do |k, v|
      v if v % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#filter_map|ERROR|#{e.message()}")
end

begin
  measure("Hash#partition", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.partition() do |k, v|
      v % 2 == 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#partition|ERROR|#{e.message()}")
end

begin
  measure("Hash#find", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.find() do |k, v|
      v < 0
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#find|ERROR|#{e.message()}")
end

begin
  measure("Hash#sum", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.sum() do |k, v|
      v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#sum|ERROR|#{e.message()}")
end

begin
  measure("Hash#delete loop", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.keys().each() do |k|
      h.delete(k)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#delete loop|ERROR|#{e.message()}")
end

begin
  measure("Hash#dup", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    h.dup()
    Time.monotonic() - start
  end
rescue e
  puts("Hash#dup|ERROR|#{e.message()}")
end

begin
  measure("Hash#[]= loop", 20000) do |n|
    h = hash_of(n)
    start = Time.monotonic()
    g = {}
    h.each() do |k, v|
      g[k] = v
    end
    Time.monotonic() - start
  end
rescue e
  puts("Hash#[]= loop|ERROR|#{e.message()}")
end

begin
  measure("String#split", 40000) do |n|
    s = text(n)
    words = s
    start = Time.monotonic()
    words.split(" ")
    Time.monotonic() - start
  end
rescue e
  puts("String#split|ERROR|#{e.message()}")
end

begin
  measure("String#lines", 40000) do |n|
    s = text(n)
    lined = s.gsub(Regexp.new(" "), "\n")
    start = Time.monotonic()
    lined.lines()
    Time.monotonic() - start
  end
rescue e
  puts("String#lines|ERROR|#{e.message()}")
end

begin
  measure("String#each_line", 40000) do |n|
    s = text(n)
    lined = s.gsub(Regexp.new(" "), "\n")
    start = Time.monotonic()
    lined.each_line() do |l|
      l
    end
    Time.monotonic() - start
  end
rescue e
  puts("String#each_line|ERROR|#{e.message()}")
end

begin
  measure("String#each_char", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.each_char() do |c|
      c
    end
    Time.monotonic() - start
  end
rescue e
  puts("String#each_char|ERROR|#{e.message()}")
end

begin
  measure("String#each_byte", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.each_byte() do |c|
      c
    end
    Time.monotonic() - start
  end
rescue e
  puts("String#each_byte|ERROR|#{e.message()}")
end

begin
  measure("String#chars", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.chars()
    Time.monotonic() - start
  end
rescue e
  puts("String#chars|ERROR|#{e.message()}")
end

begin
  measure("String#bytes", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.bytes()
    Time.monotonic() - start
  end
rescue e
  puts("String#bytes|ERROR|#{e.message()}")
end

begin
  measure("String#count", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.count("a")
    Time.monotonic() - start
  end
rescue e
  puts("String#count|ERROR|#{e.message()}")
end

begin
  measure("String#delete", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.delete("a")
    Time.monotonic() - start
  end
rescue e
  puts("String#delete|ERROR|#{e.message()}")
end

begin
  measure("String#squeeze", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.squeeze()
    Time.monotonic() - start
  end
rescue e
  puts("String#squeeze|ERROR|#{e.message()}")
end

begin
  measure("String#center", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.center(n + 10)
    Time.monotonic() - start
  end
rescue e
  puts("String#center|ERROR|#{e.message()}")
end

begin
  measure("String#chop", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.chop()
    Time.monotonic() - start
  end
rescue e
  puts("String#chop|ERROR|#{e.message()}")
end

begin
  measure("String#index", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.index_of("zzz")
    Time.monotonic() - start
  end
rescue e
  puts("String#index|ERROR|#{e.message()}")
end

begin
  measure("String#rindex", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.rindex("zzz")
    Time.monotonic() - start
  end
rescue e
  puts("String#rindex|ERROR|#{e.message()}")
end

begin
  measure("String#swapcase", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.swapcase()
    Time.monotonic() - start
  end
rescue e
  puts("String#swapcase|ERROR|#{e.message()}")
end

begin
  measure("String#partition", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.partition("zzz")
    Time.monotonic() - start
  end
rescue e
  puts("String#partition|ERROR|#{e.message()}")
end

begin
  measure("String#delete_prefix", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.delete_prefix("ab")
    Time.monotonic() - start
  end
rescue e
  puts("String#delete_prefix|ERROR|#{e.message()}")
end

begin
  measure("String#*", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s * 3
    Time.monotonic() - start
  end
rescue e
  puts("String#*|ERROR|#{e.message()}")
end

begin
  measure("String#gsub", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.gsub(Regexp.new("a"), "b")
    Time.monotonic() - start
  end
rescue e
  puts("String#gsub|ERROR|#{e.message()}")
end

begin
  measure("String#sub", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.sub(Regexp.new("a"), "b")
    Time.monotonic() - start
  end
rescue e
  puts("String#sub|ERROR|#{e.message()}")
end

begin
  measure("String#tr", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.tr("abc", "xyz")
    Time.monotonic() - start
  end
rescue e
  puts("String#tr|ERROR|#{e.message()}")
end

begin
  measure("String#scan", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.scan(Regexp.new("a"))
    Time.monotonic() - start
  end
rescue e
  puts("String#scan|ERROR|#{e.message()}")
end

begin
  measure("String#reverse", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.reverse()
    Time.monotonic() - start
  end
rescue e
  puts("String#reverse|ERROR|#{e.message()}")
end

begin
  measure("String#upcase", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.upcase()
    Time.monotonic() - start
  end
rescue e
  puts("String#upcase|ERROR|#{e.message()}")
end

begin
  measure("String#capitalize", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.capitalize()
    Time.monotonic() - start
  end
rescue e
  puts("String#capitalize|ERROR|#{e.message()}")
end

begin
  measure("String#strip", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.strip()
    Time.monotonic() - start
  end
rescue e
  puts("String#strip|ERROR|#{e.message()}")
end

begin
  measure("String#start_with?", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.start_with?("zz")
    Time.monotonic() - start
  end
rescue e
  puts("String#start_with?|ERROR|#{e.message()}")
end

begin
  measure("String#include?", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    s.include?("zzz")
    Time.monotonic() - start
  end
rescue e
  puts("String#include?|ERROR|#{e.message()}")
end

begin
  measure("String#+= loop", 8000) do |n|
    s = text(n)
    start = Time.monotonic()
    t = ""
    (n / 4).times() do |i|
      t += "x"
    end
    Time.monotonic() - start
  end
rescue e
  puts("String#+= loop|ERROR|#{e.message()}")
end


begin
  measure("StringBuilder loop", 40000) do |n|
    s = text(n)
    start = Time.monotonic()
    b = StringBuilder.new()
    n.times() do |i|
      b.append("x")
    end
    b.to_s()
    Time.monotonic() - start
  end
rescue e
  puts("StringBuilder loop|ERROR|#{e.message()}")
end

begin
  measure("interpolation loop", 20000) do |n|
    s = text(n)
    start = Time.monotonic()
    n.times() do |i|
      "item #{i}: #{i * 2}"
    end
    Time.monotonic() - start
  end
rescue e
  puts("interpolation loop|ERROR|#{e.message()}")
end


begin
  measure("JSON.stringify", 2000) do |n|
    data = (0...n).map() do |i|
      {"id": i, "name": "item", "tags": [1, 2, 3]}
    end
    start = Time.monotonic()
    JSON.stringify(data)
    Time.monotonic() - start
  end
rescue e
  puts("JSON.stringify|ERROR|#{e.message()}")
end

begin
  measure("JSON.parse", 2000) do |n|
    data = (0...n).map() do |i|
      {"id": i, "name": "item", "tags": [1, 2, 3]}
    end
    text = JSON.stringify(data)
    start = Time.monotonic()
    JSON.parse(text)
    Time.monotonic() - start
  end
rescue e
  puts("JSON.parse|ERROR|#{e.message()}")
end

begin
  measure("Time#strftime", 20000) do |n|
    t = Time.now()
    start = Time.monotonic()
    n.times() do |i|
      t.strftime("%Y-%m-%d %H:%M:%S")
    end
    Time.monotonic() - start
  end
rescue e
  puts("Time#strftime|ERROR|#{e.message()}")
end

begin
  measure("Time#iso8601", 20000) do |n|
    t = Time.now()
    start = Time.monotonic()
    n.times() do |i|
      t.iso8601()
    end
    Time.monotonic() - start
  end
rescue e
  puts("Time#iso8601|ERROR|#{e.message()}")
end

begin
  measure("Time arithmetic", 20000) do |n|
    t = Time.now()
    start = Time.monotonic()
    n.times() do |i|
      t + 86400
    end
    Time.monotonic() - start
  end
rescue e
  puts("Time arithmetic|ERROR|#{e.message()}")
end

begin
  measure("Int#times", 100000) do |n|
    start = Time.monotonic()
    n.times() do |i|
      i
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#times|ERROR|#{e.message()}")
end

begin
  measure("Int#upto", 100000) do |n|
    start = Time.monotonic()
    1.upto(n) do |i|
      i
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#upto|ERROR|#{e.message()}")
end

begin
  measure("Int#digits", 20000) do |n|
    start = Time.monotonic()
    n.times() do |i|
      i.digits()
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#digits|ERROR|#{e.message()}")
end

begin
  measure("Int#gcd", 20000) do |n|
    start = Time.monotonic()
    n.times() do |i|
      i.gcd(36)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#gcd|ERROR|#{e.message()}")
end

begin
  measure("Int#pow", 20000) do |n|
    start = Time.monotonic()
    n.times() do |i|
      i.pow(2)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#pow|ERROR|#{e.message()}")
end

begin
  measure("Int#clamp", 20000) do |n|
    start = Time.monotonic()
    n.times() do |i|
      i.clamp(0, 10)
    end
    Time.monotonic() - start
  end
rescue e
  puts("Int#clamp|ERROR|#{e.message()}")
end
