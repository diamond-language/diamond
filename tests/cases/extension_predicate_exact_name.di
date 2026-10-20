def array_big?(items) = items.length() > 2
def array_big(items) = "not the predicate"
def string_shout?(text) = text == text.upcase()
def string_wide(text) = text.length() > 3
puts([1, 2, 3].big?())
puts([1].big?())
puts([1].big())
puts("AB".shout?())
puts("abcd".wide?())
