require "../../arel/lib/arel"

# One file per class/error, each independently reopening `module
# ActiveRecord` -- possible since Diamond gained real module/class
# reopening (docs/syntax.md's "Classes" section: a second `module
# Name`/`class Name ... end` for a name that already exists adds to it
# instead of erroring). Order doesn't matter: none of these classes
# inherit from one another (the two error classes' `< StandardError` is a
# built-in, always pre-registered before user code compiles), and a
# class-name reference to another class here -- Repository#relation()
# building a Relation, Model#has_many building a HasMany, and so on --
# resolves regardless of file order too, via the compiler's own
# declaration-discovery pass (docs/roadmap.md's "Compiler representation"
# section). Kept in the same order as the original single-file layout
# purely for readability, not because it's required.
require "./active_record/validation_error"
require "./active_record/stale_object_error"
require "./active_record/instrumented_connection"
require "./active_record/repository"
require "./active_record/relation"
require "./active_record/dirty_attributes"
require "./active_record/has_many"
require "./active_record/has_one"
require "./active_record/has_many_through"
require "./active_record/belongs_to"
require "./active_record/transaction"
require "./active_record/model"
require "./active_record/migration"
require "./active_record/validators"
