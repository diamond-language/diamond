# A from-scratch, spec-inspired GraphQL query engine -- see README.md
# and ROADMAP.md for scope. One file per logical grouping, reopening
# the same `module GraphQL` (see packages/dials's own lib/dials.di for
# this repo's established convention for that).
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

require "./graphql/errors"
require "./graphql/execution/coercion"
require "./graphql/execution/directives"
require "./graphql/execution/lookahead"
require "./graphql/validation/validator"
require "./graphql/introspection"
require "./graphql/execution/executor"

# schema.di's own #execute references GraphQL::Execution::Executor by
# its fully-namespaced name -- must be required after execution/
# executor above, same cross-file forward-reference limitation
# type.di's own header comment documents (a namespaced Module::Class
# reference doesn't participate in the same-file declaration-discovery
# pass, confirmed directly for both cases).
require "./graphql/schema"
