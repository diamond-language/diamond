require "../../packages/graphql/lib/graphql"
require "../../packages/graphsql/lib/graphsql"

def map_integrated_graphsql_row(row) = row

document = GraphQL::Language::Parser.parse("{ authors { name } }")
field = document.definitions()[0].selection_set()[0]
lookahead = GraphQL::Execution::Lookahead.new(field.selection_set(), {}, {})
puts(lookahead.selections().join(","))
repository = ActiveRecord::Repository.new(
  Arel.table("authors"), map_integrated_graphsql_row, "id", nil, nil, nil, nil, nil,
  ["id", "name"])
mapping = GraphSQL::Mapping.new(repository, "Author").column("name")
planned = GraphSQL.resolve(repository.relation(), nil, lookahead, mapping)
puts(planned.query().projection_count())
