# An error raised by the runtime has the same shape as a raised Exception:
# message() is just the message, and backtrace() says where it happened.
# Re-raising an exception keeps its original backtrace.
def divide(n) = n / 0
def fail_hard()
  raise ArgumentError.new("bad")
end
runtime = begin
  divide(1)
rescue error: ZeroDivisionError
  [error.message(), error.backtrace()[0]]
end
reraised = begin
  begin
    fail_hard()
  rescue error: ArgumentError
    raise error
  end
rescue error: ArgumentError
  error.backtrace()[0]
end
[runtime, reraised]
