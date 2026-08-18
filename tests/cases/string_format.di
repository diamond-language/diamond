class Loud
  def to_s()
    "LOUD"
  end
end

puts("Name: %s, Age: %d".format(["Alice", 30]))
puts("%05d".format(42))
puts("%-10s|".format("hi"))
puts("%.2f".format(3.14159))
puts("%x %X %o %b".format([255, 255, 8, 5]))
puts("%d%%".format(50))
puts("%d".format(7))
puts("val=%s".format(Loud.new()))
puts("%d".format(3.9))
puts("no directives here".format(nil))
