# Recursive function calls -- exercises CALL overhead and stack-frame
# setup/teardown, deliberately without memoization so the call count
# is large relative to the work done per call.
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end
fib(30)
