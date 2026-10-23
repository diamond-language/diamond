module Counter
 def increment()
  if @count == nil
   @count = 1
  else
   @count = @count + 1
  end
 end
 def count() = @count
end
class Box
 include Counter
end
box = Box.new()
box.increment()
box.increment()
box.count()
