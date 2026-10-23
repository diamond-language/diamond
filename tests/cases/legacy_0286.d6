attempts = 0
cleanups = 0
begin
 attempts = attempts + 1
 if attempts < 2
  raise "again"
 end
rescue error
 retry
ensure
 cleanups = cleanups + 1
end
[attempts, cleanups]
