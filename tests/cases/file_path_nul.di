# File and Dir selectors that reach the OS reject a path with an embedded NUL.
def nul_check(index, path)
  begin
    if index == 0 then File.size(path)
    elsif index == 1 then File.mode(path)
    elsif index == 2 then File.file?(path)
    elsif index == 3 then File.symlink?(path)
    elsif index == 4 then File.exist?(path)
    elsif index == 5 then File.directory?(path)
    elsif index == 6 then Dir.mkdir(path)
    else Dir.delete(path)
    end
    "accepted"
  rescue e: TypeError
    "rejected"
  end
end

results = []
index = 0
while index < 8
  results.push(nul_check(index, "real\0junk"))
  index += 1
end
results.push(File.basename("a\0b").length())
results
