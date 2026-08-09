class Parent
 attr_reader value
 attr_writer value
end
class Child < Parent
end
child=Child.new()
child.value=(42)
child.value()
