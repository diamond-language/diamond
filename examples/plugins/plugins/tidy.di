# tidy: the same idea as peek.di -- reads ARGV[0] and reports its first
# line -- but rescues SandboxError and degrades gracefully instead of
# crashing when the host hasn't granted `filesystem`. This is the pattern
# from docs/sandbox.md's own example: a plugin author who wants their code
# to behave sensibly under a denial they can't control, rather than assume
# they'll always get the access they ask for.

def main(argv)
  path = argv[0]
  begin
    file = File.open(path, "r")
    line = file.gets()
    file.close()
    puts("first line: #{line}")
  rescue error: SandboxError
    puts("no filesystem access; skipping #{path}")
  end
  0
end

exit(main(ARGV))
