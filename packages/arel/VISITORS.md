# Arel visitor protocol

A visitor renders an Arel statement as `[sql, bind_params]`. Pass it to `statement.to_sql(visitor)` or `Arel.render(statement, visitor)`. Omitting it selects `Arel::SQLiteVisitor`.

## Implementing a visitor

Inherit from `Arel::Visitor` for shared AST traversal and validation. A concrete visitor supplies `visitor_name()`, `supports_extension?(name)`, `quote_identifier(name)`, and `render_pagination(limit, offset, params, bind_values)`. It can override `render_literal(value)` and the statement renderers for dialect-specific SQL.

The main render entry points are:

- `render(query)` for SELECT queries;
- `render_compound(query)` for compound queries;
- `render_insert(statement)`, `render_update(statement)`, and `render_delete(statement)` for writes;
- `render_expression(expression, params)` for expressions;
- `render_ctes(statement, params)` for common table expressions.

Return bind values in the same order as placeholders appear in SQL. Quote each identifier component through `quote_identifier`; it receives a validated component rather than a dotted SQL fragment. A visitor instance can be reused after an error.

## Dialects and capabilities

The included visitors are `SQLiteVisitor`, `PostgreSQLVisitor`, `MariaDBVisitor`, and `MySQLVisitor`. SQLite is the default. Select the visitor for the actual database connection, especially for pagination and write statements.

`supports_extension?(name)` controls syntax that varies by dialect. Capabilities include excluded-row attributes, conflict-target predicates, upserts, default values, `RETURNING`, explicit NULL ordering, write and recursive CTEs, integer bitwise operators, per-column defaults, and named-constraint conflict targets. Unsupported syntax raises a visitor-specific error before execution.

MySQL and MariaDB have distinct visitors: their support for `RETURNING` and their upsert syntax differ. A visitor may replace a complete write renderer when the shared SQL shape does not fit its dialect.

## Verification

Test a new visitor against its database server with SELECT, INSERT, UPDATE, DELETE, pagination, nested queries, and bind ordering. The cut includes dialect test scripts for the supported servers.
