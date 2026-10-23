attempts = 0
begin
  attempts = attempts + 1
  raise "again"
rescue
  if attempts < 2
    retry
  else
    attempts
  end
end
