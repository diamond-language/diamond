# File.join/.dirname/.basename/.extname/.absolute?/.expand_path -- pure
# path-string manipulation, no filesystem access required (the path
# need not exist), except expand_path's own getcwd() call when
# resolving a relative path with no base given.

puts(File.join("a", "b", "c"))
puts(File.join("a/", "/b/", "/c"))
puts(File.join("a", "", "b"))
puts(File.join("a"))

puts(File.dirname("/a/b/c"))
puts(File.dirname("a/b"))
puts(File.dirname("a"))
puts(File.dirname("/a"))
puts(File.dirname("/"))
puts(File.dirname("/a/b/"))
puts(File.dirname(""))

puts(File.basename("/a/b/c.rb"))
puts(File.basename("/a/b/c.rb", ".rb"))
puts(File.basename("a"))
puts(File.basename("/"))
puts(File.basename(""))
puts(File.basename("/a/b/", ".rb"))

puts(File.extname("a/b/c.rb"))
puts(File.extname(".bashrc"))
puts(File.extname("archive.tar.gz"))
puts(File.extname("noext"))
puts(File.extname("a."))

puts(File.absolute?("/a/b"))
puts(File.absolute?("a/b"))
puts(File.absolute?(""))

# Every case below uses an absolute path or an absolute base -- this
# test's own expected output must not depend on the runner's cwd (a
# relative path with no base, or a relative base, resolves against the
# real getcwd(), which isn't reproducible here).
puts(File.expand_path("/a/../b"))
puts(File.expand_path("/a/./b/../c"))
puts(File.expand_path("./x", "/a/b"))
puts(File.expand_path("../x", "/a/b"))
puts(File.expand_path("/a/../../b"))
