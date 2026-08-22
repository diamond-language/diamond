# The declaration-discovery pass (see forward_declarations.di and
# docs/roadmap.md) deliberately does NOT let a class inherit from a
# superclass declared later in the file -- compile_class copies the
# superclass's complete, final field table at the moment `class B < A`
# is parsed, which needs A already fully compiled, not just known by
# name. Pinning this down as a real, unchanged compile error (not a
# silent behavior change) is as important as the cases that now work.
class B < A
end

class A
end
