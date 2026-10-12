# DIAMOND_GRACE_WALL_MILLISECONDS bounds the time past the budget's deadline:
# a program that rescues and keeps spinning is stopped 30 ms after the 20 ms
# budget rather than a second later.
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
