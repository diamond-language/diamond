re = Regexp.new("([a-z0-9]+)=([a-z0-9]+)")
"key1=val1;key2=val2".scan(re)
