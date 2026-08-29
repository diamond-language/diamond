# Immutable SQL AST and SQLite renderer. Query nodes describe intent; only
# SQLiteVisitor knows how that intent becomes SQL.

# One file per class (a handful of shared files for the smaller AST-leaf
# node groups), each independently reopening `module Arel` -- possible
# since Diamond gained real module/class reopening (docs/syntax.md's
# "Classes" section: a second `module Name`/`class Name ... end` for a
# name that already exists adds to it instead of erroring). A class-name
# reference to another class here resolves regardless of file order via
# the compiler's own declaration-discovery pass (docs/roadmap.md's
# "Compiler representation" section) -- except for real inheritance
# (`< Super`), which still needs the superclass's file required first:
# `sqlite_visitor`/`postgresql_visitor`/`mariadb_visitor`/`mysql_visitor`
# each need `visitor` required before them, and `cte_relation` needs
# `relation_nodes` (which defines `Table`) required before it. `support`
# must come first of all: it holds the free top-level functions
# (`arel_array`, `arel_quote_identifier`, ...) used throughout, and
# Diamond's bare top-level function calls -- unlike class/module
# references -- still only resolve source order, not forward. Kept in
# the same order as the original single-file layout, which already
# satisfies every constraint above.
require "./arel/support"
require "./arel/boolean_nodes"
require "./arel/ordering_nodes"
require "./arel/expression_nodes"
require "./arel/relation_nodes"
require "./arel/join_nodes"
require "./arel/visitor"
require "./arel/sqlite_visitor"
require "./arel/postgresql_visitor"
require "./arel/query"
require "./arel/prepared_statements"
require "./arel/compound_query"
require "./arel/cte_relation"
require "./arel/write_support"
require "./arel/insert"
require "./arel/update"
require "./arel/delete"
require "./arel/mariadb_visitor"
require "./arel/mysql_visitor"
require "./arel/inspector"
require "./arel/module_functions"
