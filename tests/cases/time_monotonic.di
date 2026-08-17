t1 = Time.monotonic()
t2 = Time.monotonic()

sum = 0
i = 0
while i < 100000
  sum = sum + i
  i = i + 1
end
t3 = Time.monotonic()

[t1 is Float, t2 >= t1, t3 > t1]
