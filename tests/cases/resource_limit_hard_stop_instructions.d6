# The instruction budget raises a catchable ResourceLimitError once; a program
# that rescues it and keeps running past the grace allowance is then stopped
# for good, with an error no rescue clause can match.
def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    nil
  end
  while true
    index = index + 1
  end
end
run()
