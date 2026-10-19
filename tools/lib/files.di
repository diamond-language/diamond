# Filesystem helpers shared by the tools.

# Removes a directory and everything in it; a symlink is removed, never followed.
def remove_tree(path)
  Dir.entries(path).each() do |name|
    child = path + "/" + name
    if File.directory?(child) && !File.symlink?(child)
      remove_tree(child)
    else
      File.delete(child)
    end
  end
  Dir.delete(path)
end

# A new temporary directory.
def make_temp_dir() = Process.run(["mktemp", "-d"]).stdout().strip()
