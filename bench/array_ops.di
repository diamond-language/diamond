# Array push/index-get/index-set in a loop -- exercises INDEX_GET/
# INDEX_SET and the native .push()/.pop() path, plus GC pressure from
# array growth/reallocation.
def run()
  values = []
  index = 0
  while index < 1000000
    values.push(index)
    index = index + 1
  end
  total = 0
  index = 0
  while index < values.length()
    total = total + values[index]
    values[index] = 0
    index = index + 1
  end
  total
end
run()
