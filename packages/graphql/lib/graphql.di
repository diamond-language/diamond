# A from-scratch, spec-inspired GraphQL query engine -- see README.md
# and ROADMAP.md for scope. One file per logical grouping, reopening
# the same `module GraphQL` (see packages/dials's own lib/dials.di for
# this repo's established convention for that).
#
# TODO once the execution/validation/introspection phases land:
# require "./graphql/execution/executor"
# require "./graphql/validation/validator"
# require "./graphql/introspection"
require "./graphql/language/lexer"
require "./graphql/language/nodes"
require "./graphql/language/parser"

require "./graphql/type"
require "./graphql/scalar_type"
require "./graphql/argument"
require "./graphql/field"
require "./graphql/object_type"
require "./graphql/interface_type"
require "./graphql/union_type"
require "./graphql/enum_type"
require "./graphql/input_object_type"
require "./graphql/schema"
