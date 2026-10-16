# File.copy / size / file? / symlink? and Dir.mkdir / delete / sync.
root = Process.run(["mktemp", "-d"]).stdout().strip()
Dir.mkdir("#{root}/a", 0o700)
mode = Process.run(["stat", "-c", "%a", "#{root}/a"]).stdout().strip()
File.write("#{root}/a/f", "hello")
size = File.size("#{root}/a/f")
File.copy("#{root}/a/f", "#{root}/a/g")
copied = File.read("#{root}/a/g")

copy_refuses_to_replace = false
begin
  File.copy("#{root}/a/f", "#{root}/a/g")
rescue e: IOError
  copy_refuses_to_replace = true
end

Process.run(["ln", "-s", "#{root}/a/f", "#{root}/link"])
kinds = [File.file?("#{root}/a/f"), File.file?("#{root}/a"), File.file?("#{root}/missing"),
         File.file?("#{root}/link"), File.symlink?("#{root}/link"), File.symlink?("#{root}/a/f")]

mkdir_refuses_existing = false
begin
  Dir.mkdir("#{root}/a")
rescue e: IOError
  mkdir_refuses_existing = true
end

delete_refuses_non_empty = false
begin
  Dir.delete("#{root}/a")
rescue e: IOError
  delete_refuses_non_empty = true
end

synced = Dir.sync("#{root}/a") == "#{root}/a"
File.delete("#{root}/link")
File.delete("#{root}/a/f")
File.delete("#{root}/a/g")
Dir.delete("#{root}/a")
gone = !File.exist?("#{root}/a")
Process.run(["rm", "-rf", root])

[mode, size, copied, copy_refuses_to_replace, kinds, mkdir_refuses_existing,
 delete_refuses_non_empty, synced, gone]
