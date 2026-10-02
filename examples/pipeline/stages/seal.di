# seal: stage 3. Appends a checksum of ARGV[0] ("name=Title") and prints
# "name=Title#checksum". Pure computation, no capability.
#
# Unreliable on purpose: ARGV[1] is the attempt number the host is on.
#   - "beam" crashes on attempts 1 and 2, then succeeds: a transient fault
#     the supervisor's restart-and-retry recovers from.
#   - "poison" crashes on every attempt: the host gives up and dead-letters it.
# A crash here is an ordinary uncaught RuntimeError (exit status 1).

def checksum(text)
  sum = 0
  index = 0
  while index < text.length()
    sum = (sum * 31 + text[index].ord()) % 65521
    index = index + 1
  end
  sum
end

def main(argv)
  text = argv[0]
  attempt = argv[1].to_i()
  raise "seal failed on #{text} (attempt #{attempt})" if text.start_with?("beam") && attempt < 3
  raise "seal failed on #{text} (attempt #{attempt})" if text.start_with?("poison")
  puts("#{text}##{checksum(text)}")
  0
end

exit(main(ARGV))
