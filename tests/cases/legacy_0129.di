def outer(value)
 def captured() = value
 captured()
end
outer(42)
