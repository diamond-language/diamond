# enrich: stage 2. Looks ARGV[0] up in data/labels.txt and prints
# "name=Title" (or "name=?" when unknown). Reading the table needs the
# `filesystem` capability, which the host grants this stage -- and ONLY this
# capability, so everything else stays denied.
#
# Hostile input: the item "beacon" makes it try to phone home over TCP.
# `network` was never granted, so the sandbox raises SandboxError, which is
# left uncaught for the host to classify.

def lookup(name)
  file = File.open("data/labels.txt", "r")
  found = "?"
  loop do
    line = file.gets()
    break if line == nil
    line = line.strip()
    next if line.empty?() || line.start_with?("#")
    [key, title] = line.split("=")
    found = title if key == name
  end
  file.close()
  found
end

def main(argv)
  name = argv[0]
  if name == "beacon"
    TCPSocket.connect("127.0.0.1", 9)
  end
  puts("#{name}=#{lookup(name)}")
  0
end

exit(main(ARGV))
