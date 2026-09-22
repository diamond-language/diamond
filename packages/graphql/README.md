# graphql

Define GraphQL schemas and execute queries in Diamond.

## Installation

Install the cut at `cuts/graphql/` and load it with `require_cut "graphql"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "graphql"

module BookResolvers
  module_function
  def title(object, args, context) = object["title"]
end

module AuthorResolvers
  module_function
  def id(object, args, context) = object["id"]
  def name(object, args, context) = object["name"]
  def books(object, args, context) = object["books"]
end

module QueryResolvers
  module_function
  def author(object, args, context) = context["db"][args["id"]]
end

BookType = GraphQL::ObjectType.new("Book")
BookType.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)

AuthorType = GraphQL::ObjectType.new("Author")
AuthorType.field("id", GraphQL::ScalarType.id().non_null(), AuthorResolvers.id)
AuthorType.field("name", GraphQL::ScalarType.string().non_null(), AuthorResolvers.name)
AuthorType.field("books", GraphQL::ListType.of(BookType), AuthorResolvers.books)

QueryType = GraphQL::ObjectType.new("Query")
QueryType.field("author", AuthorType, QueryResolvers.author,
  [GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())])

Schema = GraphQL::Schema.new()
Schema.query(QueryType)

result = Schema.execute("{ author(id: \"1\") { name } }", {}, {"db": db})
```

## Notes

Define types and fields with builders, then call `schema.execute(query, variables, context, root_value, operation_name)`. Results contain `data` and, when applicable, `errors`.
