# begin/rescue/ensure overhead in a tight loop -- exercises
# PUSH_RESCUE/POP_RESCUE/PUSH_ENSURE/RUN_ENSURE/END_ENSURE/RAISE, none
# of which any other benchmark in this suite touches. Every iteration
# both raises and rescues (the common "expected, handled" case, not an
# uncaught-exception crash path) and runs an ensure block, so this
# measures the steady-state cost of exception machinery itself, not
# just the rare-path cost of actually unwinding.
def run()
  total = 0
  index = 0
  while index < 500000
    begin
      begin
        raise RuntimeError.new("boom") if index % 2 == 0
        total = total + 1
      rescue error: RuntimeError
        total = total + 2
      end
    ensure
      total = total + 1
    end
    index = index + 1
  end
  total
end
run()
