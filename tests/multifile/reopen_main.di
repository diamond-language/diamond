require "reopen_a"
require "reopen_b"

w = Shared::Widget.new()
extra = Consumer.new().make()
[w.a(), w.b(), extra.hi()]
