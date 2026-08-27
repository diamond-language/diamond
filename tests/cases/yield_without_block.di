# run! exit=1 contains=expected Callable, got Nil
def invoke(&block)
  yield(1)
end

invoke()
