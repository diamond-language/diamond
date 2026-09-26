# flush hands buffered writes to the OS before close; exist? reports
# whether anything is at a path without raising.
path = "/tmp/diamond-flush-#{SecureRandom.hex(8)}"
before = File.exist?(path)
writer = File.open(path, "w")
writer.write("first")
flushed = writer.flush() == writer
reader = File.open(path, "r")
seen = reader.read()
reader.close()
writer.close()
after = File.exist?(path)
File.delete(path)
[before, flushed, seen, after, File.exist?(path), File.exist?("/tmp")]
