def string_shout(text) = text.upcase()

[
  "abc".respond_to?(:length), "abc".respond_to?("upcase"), "abc".respond_to?(:shout),
  "abc".respond_to?(:fly), [1].respond_to?(:push), [1].respond_to?(:each_slice),
  [1].respond_to?(:fly), {"a": 1}.respond_to?(:keys), {}.respond_to?(:fly),
  5.respond_to?(:times), 5.respond_to?(:fly), 2.5.respond_to?(:round), 2.5.respond_to?(:fly),
  "abc".respond_to?(:respond_to?), "abc".respond_to?(:each_byte), "abc".respond_to?(:unpack)
]
