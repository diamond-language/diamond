# spinloop: a plugin that never finishes on its own, standing in for a
# buggy or hostile one. Touches nothing outside the process -- sandbox mode
# alone would let it run forever -- so it's the resource limits
# (DIAMOND_MAX_INSTRUCTIONS / DIAMOND_MAX_WALL_MILLISECONDS) that stop it,
# each raising a rescuable ResourceLimitError. Left uncaught here, the same
# way peek.di leaves SandboxError uncaught, so the host sees a crash.

def main(argv)
  total = 0
  while true
    total = total + 1
  end
  total
end

exit(main(ARGV))
