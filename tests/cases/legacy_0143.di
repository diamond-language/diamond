class User
 def initialize(name)
  @name = name
 end
 def to_s() -> String = "User(#{@name})"
end
"hello #{User.new("Ada")}"
