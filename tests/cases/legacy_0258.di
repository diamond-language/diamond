module Query
 def valid?() = true
end
class Box
 include Query
end
Box.new().valid?()
