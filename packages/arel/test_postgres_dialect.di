# Conformance suite for Arel::PostgreSQLVisitor -- Arel's second dialect (see
# ROADMAP.md's "pick the next dialect" milestone and this visitor's own
# class-level comment in lib/arel.di). Requires an already-running
# PostgreSQL server; Diamond can't spin one up itself the way it can a local
# TCPServer, so unlike every fixture under tests/cases/ (all self-contained,
# in-memory SQLite), this intentionally lives outside that corpus and is
# opt-in -- run via test_postgres_dialect.sh, which manages its own
# throwaway container. Connection info comes from DIAMOND_PG_TEST_CONNINFO;
# see that script for how it's set.
require "../../lib/minitest"
require "./lib/arel"

def run_tests()
  conninfo = ENV["DIAMOND_PG_TEST_CONNINFO"]
  if conninfo == nil
    raise RuntimeError.new(
      "DIAMOND_PG_TEST_CONNINFO is not set -- run via test_postgres_dialect.sh")
  end
  visitor = Arel::PostgreSQLVisitor.new()

  def test_portable_baseline_select_where_order_join(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_books")
    db.execute("DROP TABLE IF EXISTS pg_dialect_authors")
    db.execute("CREATE TABLE pg_dialect_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")
    db.execute("CREATE TABLE pg_dialect_books (id SERIAL PRIMARY KEY, title TEXT, author_id INTEGER)")
    db.execute("INSERT INTO pg_dialect_authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
    db.execute("INSERT INTO pg_dialect_authors (name, country) VALUES (?, ?)", ["Grace", "USA"])
    db.execute("INSERT INTO pg_dialect_books (title, author_id) VALUES (?, ?)", ["Book A", 1])
    db.execute("INSERT INTO pg_dialect_books (title, author_id) VALUES (?, ?)", ["Book B", 2])

    authors = Arel.table("pg_dialect_authors")
    books = Arel.table("pg_dialect_books")

    rows = Arel.from(authors).where(authors.column("country").eq("UK")).to_a(db, visitor)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0]["name"])

    ordered = Arel.from(authors).order(authors.column("name").asc()).to_a(db, visitor)
    Minitest.assert_equal("Ada", ordered[0]["name"])
    Minitest.assert_equal("Grace", ordered[1]["name"])

    joined = Arel.from(books).join(authors, books.column("author_id").eq(authors.column("id")))
    joined = joined.project([books.column("title"), authors.column("name")])
    joined_rows = joined.order(books.column("id").asc()).to_a(db, visitor)
    Minitest.assert_equal(2, joined_rows.length())
    Minitest.assert_equal("Book A", joined_rows[0]["title"])
    Minitest.assert_equal("Ada", joined_rows[0]["name"])

    db.close()
  end

  def test_compound_queries(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_nums")
    db.execute("CREATE TABLE pg_dialect_nums (value INTEGER)")
    db.execute("INSERT INTO pg_dialect_nums (value) VALUES (1), (2), (2), (3)")
    nums = Arel.table("pg_dialect_nums")

    left = Arel.from(nums).where(nums.column("value").lt(3)).project([nums.column("value")])
    right = Arel.from(nums).where(nums.column("value").gt(1)).project([nums.column("value")])

    union_rows = Arel.union(left, right).to_a(db, visitor)
    Minitest.assert_equal(3, union_rows.length())

    union_all_rows = Arel.union_all(left, right).to_a(db, visitor)
    Minitest.assert_equal(6, union_all_rows.length())

    intersect_rows = Arel.intersect(left, right).to_a(db, visitor)
    Minitest.assert_equal(1, intersect_rows.length())
    Minitest.assert_equal(2, intersect_rows[0]["value"])

    except_rows = Arel.except(left, right).to_a(db, visitor)
    Minitest.assert_equal(1, except_rows.length())
    Minitest.assert_equal(1, except_rows[0]["value"])

    db.close()
  end

  def test_non_recursive_cte(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_orders")
    db.execute("CREATE TABLE pg_dialect_orders (id SERIAL PRIMARY KEY, qty INTEGER)")
    db.execute("INSERT INTO pg_dialect_orders (qty) VALUES (5), (15), (25)")
    orders = Arel.table("pg_dialect_orders")

    large = Arel.from(orders).where(orders.column("qty").gt(10))
    rows = Arel.from(Arel.table("large_orders")).with("large_orders", large).to_a(db, visitor)
    Minitest.assert_equal(2, rows.length())

    db.close()
  end

  def test_pagination_limit_and_bare_offset(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_page")
    db.execute("CREATE TABLE pg_dialect_page (n INTEGER)")
    db.execute("INSERT INTO pg_dialect_page (n) VALUES (1), (2), (3), (4), (5)")
    page = Arel.table("pg_dialect_page")
    base = Arel.from(page).order(page.column("n").asc())

    limited = base.take(2)
    sql, params = limited.to_sql(visitor)
    Minitest.assert_equal(false, sql.include?("-1"))
    rows = limited.to_a(db, visitor)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(1, rows[0]["n"])

    offset_only = base.skip(3)
    offset_sql, offset_params = offset_only.to_sql(visitor)
    Minitest.assert_equal(false, offset_sql.include?("LIMIT"))
    offset_rows = offset_only.to_a(db, visitor)
    Minitest.assert_equal(2, offset_rows.length())
    Minitest.assert_equal(4, offset_rows[0]["n"])

    both_rows = base.take(2).skip(1).to_a(db, visitor)
    Minitest.assert_equal(2, both_rows.length())
    Minitest.assert_equal(2, both_rows[0]["n"])

    db.close()
  end

  def test_insert_update_delete_and_returning(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_items")
    db.execute("CREATE TABLE pg_dialect_items (id SERIAL PRIMARY KEY, name TEXT, qty INTEGER)")
    items = Arel.table("pg_dialect_items")

    insert = Arel.insert_into(items).values({"name": "pens", "qty": 4})
    insert = insert.returning([items.column("id")])
    inserted_rows = insert.to_a(db, visitor)
    Minitest.assert_equal(1, inserted_rows[0]["id"])

    update = Arel.update(items).set({"qty": 9})
    update = update.where(items.column("name").eq("pens"))
    Minitest.assert_equal(1, update.execute(db, visitor))
    Minitest.assert_equal(9, db.query("SELECT qty FROM pg_dialect_items")[0]["qty"])

    deletion = Arel.delete_from(items).where(items.column("name").eq("pens"))
    Minitest.assert_equal(1, deletion.execute(db, visitor))
    Minitest.assert_equal(0, db.query("SELECT * FROM pg_dialect_items").length())

    db.close()
  end

  def test_insert_default_values_and_insert_select(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_jobs")
    db.execute("DROP TABLE IF EXISTS pg_dialect_job_names")
    db.execute(
      "CREATE TABLE pg_dialect_jobs (id SERIAL PRIMARY KEY, state TEXT DEFAULT 'queued')")
    db.execute("CREATE TABLE pg_dialect_job_names (name TEXT)")
    jobs = Arel.table("pg_dialect_jobs")
    job_names = Arel.table("pg_dialect_job_names")

    insert = Arel.insert_into(jobs).default_values()
    insert = insert.returning([jobs.column("state")])
    rows = insert.to_a(db, visitor)
    Minitest.assert_equal("queued", rows[0]["state"])

    db.execute("INSERT INTO pg_dialect_job_names (name) VALUES (?)", ["queued"])
    select_source = Arel.from(job_names).project([job_names.column("name")])
    insert_select = Arel.insert_into(jobs).from_query(["state"], select_source)
    Minitest.assert_equal(1, insert_select.execute(db, visitor))
    Minitest.assert_equal(2, db.query("SELECT * FROM pg_dialect_jobs").length())

    db.close()
  end

  def test_write_cte_and_recursive_cte(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_stock")
    db.execute("CREATE TABLE pg_dialect_stock (id SERIAL PRIMARY KEY, qty INTEGER)")
    db.execute("INSERT INTO pg_dialect_stock (qty) VALUES (1), (2), (3)")
    stock = Arel.table("pg_dialect_stock")

    source = Arel.from(stock).where(stock.column("qty").gt(1))
    update = Arel.update(stock).with("selected", source).set({"qty": 9})
    update = update.where(stock.column("id").in_subquery(
      Arel.from(Arel.table("selected")).project([Arel.table("selected").column("id")])))
    Minitest.assert_equal(2, update.execute(db, visitor))

    numbers = Arel.table("pg_dialect_numbers")
    seed = Arel.from(stock).where(stock.column("id").eq(1)).project([Arel.as(Arel.sql("1"), "value")])
    step = Arel.from(numbers).project([Arel.sql("value + 1")])
    step = step.where(numbers.column("value").lt(3))
    body = Arel.union_all(seed, step)
    recursive_rows = Arel.from(numbers).with_recursive("pg_dialect_numbers", body).to_a(db, visitor)
    Minitest.assert_equal(3, recursive_rows.length())

    db.close()
  end

  def test_on_conflict_do_nothing_and_do_update(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_inventory")
    db.execute("CREATE TABLE pg_dialect_inventory (name TEXT UNIQUE, qty INTEGER)")
    inventory = Arel.table("pg_dialect_inventory")

    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_nothing(["name"])
    Minitest.assert_equal(1, insert.execute(db, visitor))
    Minitest.assert_equal(0, insert.execute(db, visitor))
    Minitest.assert_equal(1, db.query("SELECT * FROM pg_dialect_inventory").length())

    upsert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    upsert = upsert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    upsert.execute(db, visitor)
    Minitest.assert_equal(4, db.query("SELECT qty FROM pg_dialect_inventory")[0]["qty"])

    db.execute("DROP TABLE IF EXISTS pg_dialect_users")
    db.execute("CREATE TABLE pg_dialect_users (email TEXT, active INTEGER)")
    db.execute("CREATE UNIQUE INDEX pg_dialect_active_email ON pg_dialect_users(email) WHERE active = 1")
    users = Arel.table("pg_dialect_users")
    target = Arel.conflict_target(["email"])
    target = target.where(target.column("active").eq(Arel.literal(1)))
    partial = Arel.insert_into(users).values({"email": "a@example.test", "active": 1})
    partial = partial.on_conflict_do_nothing(target)
    Minitest.assert_equal(1, partial.execute(db, visitor))
    Minitest.assert_equal(0, partial.execute(db, visitor))
    Minitest.assert_equal(1, db.query("SELECT * FROM pg_dialect_users").length())

    db.close()
  end

  def test_named_constraint_conflict_target(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_named_conflict")
    db.execute(
      "CREATE TABLE pg_dialect_named_conflict (" +
      "name TEXT, qty INTEGER, CONSTRAINT pg_dialect_named_conflict_name_key UNIQUE (name))")
    items = Arel.table("pg_dialect_named_conflict")
    target = Arel.conflict_target_on_constraint("pg_dialect_named_conflict_name_key")

    insert = Arel.insert_into(items).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_nothing(target)
    Minitest.assert_equal(1, insert.execute(db, visitor))
    Minitest.assert_equal(0, insert.execute(db, visitor))

    upsert = Arel.insert_into(items).values({"name": "pens", "qty": 4})
    upsert = upsert.on_conflict_do_update(target, {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    upsert.execute(db, visitor)
    Minitest.assert_equal(4, db.query("SELECT qty FROM pg_dialect_named_conflict")[0]["qty"])

    db.close()
  end

  def test_column_default_in_multi_row_insert(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_column_default")
    db.execute(
      "CREATE TABLE pg_dialect_column_default (name TEXT, qty INTEGER DEFAULT 7)")
    items = Arel.table("pg_dialect_column_default")

    insert = Arel.insert_into(items).values_many([
      {"name": "pens", "qty": 4},
      {"name": "pencils", "qty": Arel.column_default()}
    ])
    Minitest.assert_equal(2, insert.execute(db, visitor))
    rows = Arel.from(items).order(items.column("name").asc()).to_a(db, visitor)
    Minitest.assert_equal("pencils", rows[0]["name"])
    Minitest.assert_equal(7, rows[0]["qty"])
    Minitest.assert_equal("pens", rows[1]["name"])
    Minitest.assert_equal(4, rows[1]["qty"])

    db.close()
  end

  def test_nulls_first_and_nulls_last(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_scores")
    db.execute("CREATE TABLE pg_dialect_scores (score INTEGER)")
    db.execute("INSERT INTO pg_dialect_scores (score) VALUES (1), (NULL), (2)")
    scores = Arel.table("pg_dialect_scores")

    first = Arel.asc(scores.column("score")).nulls_first()
    first_rows = Arel.from(scores).order(first).to_a(db, visitor)
    Minitest.assert_equal(nil, first_rows[0]["score"])

    last = Arel.asc(scores.column("score")).nulls_last()
    last_rows = Arel.from(scores).order(last).to_a(db, visitor)
    Minitest.assert_equal(nil, last_rows[2]["score"])

    db.close()
  end

  # Constructs real Arel::BinaryExpression nodes via Arel.integer_operator
  # (not the Arel.sql raw escape hatch, which would bypass
  # render_expression_extension's require_extension("integer bitwise
  # operators") check entirely and prove nothing about the capability
  # this visitor claims) -- same construction arel_integer_execution.di
  # already uses against SQLite.
  def test_integer_bitwise_operators(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_masks")
    db.execute("CREATE TABLE pg_dialect_masks (value INTEGER)")
    db.execute("INSERT INTO pg_dialect_masks (value) VALUES (13)")
    masks = Arel.table("pg_dialect_masks")
    query = Arel.from(masks).project([
      Arel.as(Arel.integer_operator(masks.column("value"), "&", 6), "and_value"),
      Arel.as(Arel.integer_operator(masks.column("value"), "|", 2), "or_value"),
      Arel.as(Arel.integer_operator(masks.column("value"), "<<", 1), "left_value"),
      Arel.as(Arel.integer_operator(masks.column("value"), ">>", 2), "right_value")
    ])
    row = query.to_a(db, visitor)[0]
    Minitest.assert_equal(4, row["and_value"])
    Minitest.assert_equal(15, row["or_value"])
    Minitest.assert_equal(26, row["left_value"])
    Minitest.assert_equal(3, row["right_value"])
    db.close()
  end

  def test_identifier_quoting_with_embedded_quote(conninfo, visitor)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS pg_dialect_quoting")
    db.execute("CREATE TABLE pg_dialect_quoting (\"na\"\"me\" TEXT)")
    quoting = Arel.table("pg_dialect_quoting")
    insert = Arel.insert_into(quoting).values({"na\"me": "value"})
    insert.execute(db, visitor)
    rows = Arel.from(quoting).where(quoting.column("na\"me").eq("value")).to_a(db, visitor)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("value", rows[0]["na\"me"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("portable baseline: select/where/order/join") do
    test_portable_baseline_select_where_order_join(conninfo, visitor)
  end
  suite.test("compound queries: union/union_all/intersect/except") do
    test_compound_queries(conninfo, visitor)
  end
  suite.test("non-recursive CTE") do
    test_non_recursive_cte(conninfo, visitor)
  end
  suite.test("pagination: LIMIT and bare OFFSET") do
    test_pagination_limit_and_bare_offset(conninfo, visitor)
  end
  suite.test("INSERT/UPDATE/DELETE and RETURNING") do
    test_insert_update_delete_and_returning(conninfo, visitor)
  end
  suite.test("INSERT DEFAULT VALUES and INSERT SELECT") do
    test_insert_default_values_and_insert_select(conninfo, visitor)
  end
  suite.test("write CTE and recursive CTE") do
    test_write_cte_and_recursive_cte(conninfo, visitor)
  end
  suite.test("ON CONFLICT DO NOTHING / DO UPDATE") do
    test_on_conflict_do_nothing_and_do_update(conninfo, visitor)
  end
  suite.test("named-constraint conflict target") do
    test_named_constraint_conflict_target(conninfo, visitor)
  end
  suite.test("per-column DEFAULT in multi-row INSERT") do
    test_column_default_in_multi_row_insert(conninfo, visitor)
  end
  suite.test("NULLS FIRST / NULLS LAST") do
    test_nulls_first_and_nulls_last(conninfo, visitor)
  end
  suite.test("integer bitwise operators") do
    test_integer_bitwise_operators(conninfo, visitor)
  end
  suite.run!()
end

run_tests()
