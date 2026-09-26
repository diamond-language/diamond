# File.rename replaces an existing destination and returns it.
dir = "/tmp/diamond-file-rename-#{SecureRandom.hex(8)}"
old = "#{dir}-old"
new = "#{dir}-new"
def write_file(path: String, text: String)
  file = File.open(path, "w")
  file.write(text)
  file.close()
end
write_file(old, "replacement")
write_file(new, "original")
returned = File.rename(old, new)
reader = File.open(new, "r")
contents = reader.read()
reader.close()
missing = begin
  File.rename(old, new)
rescue error: IOError
  "gone"
end
File.delete(new)
[returned == new, contents, missing]
