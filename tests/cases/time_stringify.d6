t = Time.at(0).utc()
puts(t)
puts("interpolated: #{t}")
t  # bare trailing expression: exercises the CLI's own top-level-result
   # auto-print (src/value.c's diamond_value_fprint), a separate path
   # from puts/interpolation above -- it had no Time case at all until
   # a real formatted date was shared into it (see CHANGELOG)
