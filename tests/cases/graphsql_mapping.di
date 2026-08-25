require "../../packages/graphsql/lib/graphsql"
require "../../lib/minitest"

def map_graphsql_row(row) = row

def run_tests()
  repository = ActiveRecord::Repository.new(
    Arel.table("authors"), map_graphsql_row, "id", nil, nil, nil, nil, nil,
    ["id", "name", "country"])
  parent = GraphSQL::Mapping.new(repository, "Node").column("id")
  mapping = GraphSQL::Mapping.new(repository, "Author", parent)
  mapping.column("name").column("country", "homeCountry")
  mapping.association("books").association("organization", "employer", parent)

  suite = Minitest.new()
  suite.test("column mappings include inherited and aliased fields") do
    Minitest.assert_equal("id", mapping.columns()["id"])
    Minitest.assert_equal("name", mapping.columns()["name"])
    Minitest.assert_equal("country", mapping.columns()["homeCountry"])
  end
  suite.test("association mappings retain field, association, and target") do
    Minitest.assert_equal("books", mapping.associations()["books"].name())
    Minitest.assert_equal("organization", mapping.associations()["employer"].name())
    Minitest.assert_equal("Node", mapping.associations()["employer"].target().type_name())
  end
  suite.test("child mappings override inherited fields") do
    child = GraphSQL::Mapping.new(repository, "SpecialAuthor", mapping).column("country", "name")
    Minitest.assert_equal("country", child.columns()["name"])
  end
  suite.run!()
end

run_tests()
