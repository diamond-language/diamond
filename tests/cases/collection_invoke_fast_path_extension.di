# A program-defined array_push replaces Array#push. The call-site fast path
# for #push (collection_invoke_fast in src/vm.c) must still honour that, on
# the first call (cold extension cache) and on every later one. No Minitest
# here: it registers its tests with Array#push, which this file replaces.
def array_push(items, value)
  "extended"
end

def run_tests()
  values = [1]
  result = nil
  index = 0
  while index < 3
    result = values.push(index)
    index = index + 1
  end
  puts(result)
  puts(values.length())
end

run_tests()
