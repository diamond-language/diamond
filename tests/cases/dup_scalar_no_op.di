# dup on a primitive/immutable value is a no-op self-return, matching
# Ruby's own Integer#dup/String#dup.
"#{5.dup()}, #{"abc".dup()}"
