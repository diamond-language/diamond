Process.run(["rm", "-rf", "/tmp/diamond_dir_entries_test"])
Process.run(["mkdir", "-p", "/tmp/diamond_dir_entries_test"])
Process.run(["touch", "/tmp/diamond_dir_entries_test/a.txt"])
Process.run(["touch", "/tmp/diamond_dir_entries_test/b.txt"])

entries = Dir.entries("/tmp/diamond_dir_entries_test").sort_by() do |e| e end

missing_rejected = false
begin
  Dir.entries("/tmp/diamond_dir_entries_test/does_not_exist")
rescue error: IOError
  missing_rejected = true
end

Process.run(["rm", "-rf", "/tmp/diamond_dir_entries_test"])

[entries, missing_rejected]
