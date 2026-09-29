# peek: reads ARGV[0] and prints its first line. Makes no attempt to
# handle being denied -- crashes with an uncaught SandboxError if the host
# hasn't granted this plugin the `filesystem` capability. Contrast with
# tidy.di, which rescues the same denial gracefully.

def main(argv)
  path = argv[0]
  file = File.open(path, "r")
  puts("first line: #{file.gets()}")
  file.close()
  0
end

exit(main(ARGV))
