# normalize: stage 1. Trims and lowercases ARGV[0]. Needs no capability, so
# the host grants none. ARGV[1] is the attempt number, unused here.
#
# Hostile input: the item "spin" sends this stage into an endless loop. Sandbox
# mode alone would let that run forever -- only the host's resource budget
# (DIAMOND_MAX_INSTRUCTIONS) stops it, with an uncaught ResourceLimitError.

def main(argv)
  item = argv[0].strip().downcase()
  if item == "spin"
    total = 0
    while true
      total = total + 1
    end
  end
  puts(item)
  0
end

exit(main(ARGV))
