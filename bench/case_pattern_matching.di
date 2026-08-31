# case/when dispatch over a plain Int scrutinee -- exercises
# CASE_MATCH, untouched by every other benchmark in this suite (the
# dispatch_* benchmarks all cover method-call dispatch, not pattern
# matching). Cycles through every arm plus the else fallthrough so no
# single branch's inline cache/comparison dominates the count.
def classify(value)
  case value
  when 0
    "zero"
  when 1, 2, 3
    "small"
  when 4..10
    "medium"
  else
    "large"
  end
end

def run()
  total = 0
  index = 0
  while index < 1000000
    result = classify(index % 12)
    total = total + result.length()
    index = index + 1
  end
  total
end
run()
