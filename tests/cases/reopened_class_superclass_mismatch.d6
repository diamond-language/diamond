# A reopen naming a *different* superclass than the one already
# established errors instead of silently changing what the class
# inherits from -- matches Ruby's own TypeError: superclass mismatch in
# spirit. (Introducing a superclass for the first time via a later
# reopen, after the class started with none, is treated the same way --
# see docs/syntax.md's "Classes" section.)
class BaseA
end

class BaseB
end

class Sub < BaseA
end

class Sub < BaseB
end
