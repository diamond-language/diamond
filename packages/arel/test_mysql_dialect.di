# Conformance suite for Arel::MySQLVisitor -- Arel's fourth dialect (see
# ROADMAP.md's "pick the next dialect" entry and this visitor's own
# class-level comment in lib/arel.di). Requires an already-running MySQL
# server; Diamond can't spin one up itself the way it can a local
# TCPServer, so unlike every fixture under tests/cases/ (all self-contained,
# in-memory SQLite), this intentionally lives outside that corpus and is
# opt-in -- run via test_mysql_dialect.sh, which manages its own throwaway
# container. Connection info comes from five DIAMOND_MYSQL_TEST_* env vars
# (MySQL.open takes discrete arguments, not one conninfo String) -- the
# same native class and the same env var names test_mariadb_dialect.di
# already uses, since MySQL.open was never MariaDB-specific to begin with
# (see docs/io.md); see test_mysql_dialect.sh for how they're set.
require "../../lib/minitest"
require "./lib/arel"

def run_tests()
  if ENV["DIAMOND_MYSQL_TEST_HOST"] == nil
    raise RuntimeError.new(
      "DIAMOND_MYSQL_TEST_HOST is not set -- run via test_mysql_dialect.sh")
  end
  # Bundled into one Array local rather than five separate ones, and no
  # separate `host` local either -- nested functions and blocks capture
  # every outer local unconditionally (capped at 16, see src/compiler.c),
  # and this file's dozen nested test_* helpers already use most of that
  # headroom.
  conn = [ENV["DIAMOND_MYSQL_TEST_HOST"], ENV["DIAMOND_MYSQL_TEST_USER"],
    ENV["DIAMOND_MYSQL_TEST_PASSWORD"], ENV["DIAMOND_MYSQL_TEST_DATABASE"],
    ENV["DIAMOND_MYSQL_TEST_PORT"].to_i()]
  visitor = Arel::MySQLVisitor.new()

  def open_db(conn) = MySQL.open(conn[0], conn[1], conn[2], conn[3], conn[4])

  def test_select_where_join_order(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_books")
    db.execute("DROP TABLE IF EXISTS my_authors")
    db.execute("CREATE TABLE my_authors (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, country TEXT)")
    db.execute("CREATE TABLE my_books (id INT PRIMARY KEY AUTO_INCREMENT, title TEXT, author_id INT)")
    db.execute("INSERT INTO my_authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
    db.execute("INSERT INTO my_authors (name, country) VALUES (?, ?)", ["Grace", "USA"])
    db.execute("INSERT INTO my_books (title, author_id) VALUES (?, ?)", ["Book A", 1])
    db.execute("INSERT INTO my_books (title, author_id) VALUES (?, ?)", ["Book B", 2])

    authors = Arel.table("my_authors")
    books = Arel.table("my_books")

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

  def test_pagination(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_nums")
    db.execute("CREATE TABLE my_nums (n INT)")
    db.execute("INSERT INTO my_nums (n) VALUES (1),(2),(3),(4),(5)")
    nums = Arel.table("my_nums")
    limited = Arel.from(nums).order(nums.column("n").asc()).take(2).to_a(db, visitor)
    Minitest.assert_equal(2, limited.length())
    Minitest.assert_equal(1, limited[0]["n"])
    offset_only = Arel.from(nums).order(nums.column("n").asc()).skip(3).to_a(db, visitor)
    Minitest.assert_equal(2, offset_only.length())
    Minitest.assert_equal(4, offset_only[0]["n"])
    both = Arel.from(nums).order(nums.column("n").asc()).take(2).skip(1).to_a(db, visitor)
    Minitest.assert_equal(2, both.length())
    Minitest.assert_equal(2, both[0]["n"])
    db.close()
  end

  # Real MySQL 8 has no RETURNING at all (unlike MariaDB, which has it on
  # INSERT/DELETE) -- verified directly against a live server. All three
  # statement kinds reject it through the same shared "returning clauses"
  # gate (Visitor#render_returning), so this checks all three instead of
  # test_mariadb_dialect.di's own split (INSERT/DELETE work there, only
  # UPDATE is rejected).
  def test_returning_rejected_everywhere(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_items")
    db.execute("CREATE TABLE my_items (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, qty INT)")
    items = Arel.table("my_items")

    insert_message = nil
    begin
      Arel.insert_into(items).values({"name": "pens", "qty": 4}).returning(
        [items.column("id")]).to_sql(visitor)
    rescue error: ArgumentError
      insert_message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support returning clauses", insert_message)

    db.execute("INSERT INTO my_items (name, qty) VALUES (?, ?)", ["pens", 4])

    update_message = nil
    begin
      Arel.update(items).set({"qty": 9}).where(items.column("name").eq("pens")).returning(
        [items.column("id")]).to_sql(visitor)
    rescue error: ArgumentError
      update_message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support returning clauses", update_message)

    delete_message = nil
    begin
      Arel.delete_from(items).where(items.column("name").eq("pens")).returning(
        [items.column("id")]).to_sql(visitor)
    rescue error: ArgumentError
      delete_message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support returning clauses", delete_message)

    # Ordinary (RETURNING-less) writes still work fine, same visitor.
    updated = Arel.update(items).set({"qty": 9}).where(items.column("name").eq("pens")).execute(db, visitor)
    Minitest.assert_equal(1, updated)
    deleted = Arel.delete_from(items).where(items.column("name").eq("pens")).execute(db, visitor)
    Minitest.assert_equal(1, deleted)
    db.close()
  end

  def test_default_values_and_column_default(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_jobs")
    # VARCHAR, not TEXT (unlike test_mariadb_dialect.di's own schema) --
    # real MySQL 8 rejects a bare TEXT/BLOB column DEFAULT outright
    # ("BLOB, TEXT, GEOMETRY or JSON column 'state' can't have a default
    # value"), verified directly; MariaDB allows it. A DDL/schema
    # difference, not an Arel-visitor rendering difference -- Arel never
    # generates CREATE TABLE.
    db.execute("CREATE TABLE my_jobs (id INT PRIMARY KEY AUTO_INCREMENT, state VARCHAR(50) DEFAULT 'queued')")
    jobs = Arel.table("my_jobs")
    # No RETURNING on MySQL -- read the generated row back via the last
    # insert id instead, the realistic way a RETURNING-less dialect is
    # actually used (db.last_insert_row_id(), docs/io.md).
    Arel.insert_into(jobs).default_values().execute(db, visitor)
    new_id = db.last_insert_row_id()
    row = Arel.from(jobs).where(jobs.column("id").eq(new_id)).to_a(db, visitor)
    Minitest.assert_equal("queued", row[0]["state"])

    multi = Arel.insert_into(jobs).values_many([
      {"id": 100, "state": "custom"},
      {"id": 101, "state": Arel.column_default()}
    ])
    Minitest.assert_equal(2, multi.execute(db, visitor))
    rows = Arel.from(jobs).where(jobs.column("id").gt(99)).order(jobs.column("id").asc()).to_a(db, visitor)
    Minitest.assert_equal("custom", rows[0]["state"])
    Minitest.assert_equal("queued", rows[1]["state"])
    db.close()
  end

  def test_upsert_ignore_and_update(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_inventory")
    # VARCHAR, not TEXT -- real MySQL 8 rejects a bare TEXT/BLOB column in
    # a key/UNIQUE specification without an explicit prefix length,
    # verified directly; MariaDB allows it. Same DDL-only difference as
    # test_default_values_and_column_default's own comment above.
    db.execute("CREATE TABLE my_inventory (name VARCHAR(50) UNIQUE, qty INT)")
    inventory = Arel.table("my_inventory")

    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_nothing(["name"])
    Minitest.assert_equal(1, insert.execute(db, visitor))
    # Same MySQL/MariaDB affected-rows convention as test_mariadb_dialect.di
    # -- a duplicate INSERT IGNORE reports 0, verified directly.
    Minitest.assert_equal(0, insert.execute(db, visitor))
    Minitest.assert_equal(1, db.query("SELECT * FROM my_inventory").length())

    upsert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    upsert = upsert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    # Renders MySQL's modern row-alias form (`... AS new_row ON DUPLICATE
    # KEY UPDATE qty = new_row.qty`), not MariaDB's deprecated `VALUES()`
    # form -- verified live to behave identically either way.
    upsert.execute(db, visitor)
    Minitest.assert_equal(4, db.query("SELECT qty FROM my_inventory")[0]["qty"])
    db.close()
  end

  def test_upsert_target_shape_rejections(visitor)
    items = Arel.table("my_reject_items")
    target = Arel.conflict_target(["name"])
    target = target.where(target.column("active").eq(Arel.literal(1)))
    insert = Arel.insert_into(items).values({"name": "pens", "active": 1})
    insert = insert.on_conflict_do_nothing(target)
    message = nil
    begin
      insert.to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support conflict-target predicates", message)

    insert2 = Arel.insert_into(items).values({"name": "pens"})
    insert2 = insert2.on_conflict_do_nothing(Arel.conflict_target_on_constraint("items_name_key"))
    message2 = nil
    begin
      insert2.to_sql(visitor)
    rescue error: ArgumentError
      message2 = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support named-constraint conflict targets", message2)
  end

  def test_read_and_recursive_cte(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_stock")
    db.execute("CREATE TABLE my_stock (id INT PRIMARY KEY AUTO_INCREMENT, qty INT)")
    db.execute("INSERT INTO my_stock (qty) VALUES (1), (2), (3)")
    stock = Arel.table("my_stock")
    cte = Arel.cte("selected")
    source = Arel.from(stock).where(stock.column("qty").gt(1))
    query = Arel.from(cte).with(cte, source)
    rows = query.to_a(db, visitor)
    Minitest.assert_equal(2, rows.length())

    numbers = Arel.table("my_numbers")
    seed = Arel.from(stock).where(stock.column("id").eq(1)).project([Arel.as(Arel.sql("1"), "value")])
    step = Arel.from(numbers).project([Arel.sql("value + 1")])
    step = step.where(numbers.column("value").lt(3))
    body = Arel.union_all(seed, step)
    recursive_rows = Arel.from(numbers).with_recursive("my_numbers", body).to_a(db, visitor)
    Minitest.assert_equal(3, recursive_rows.length())
    db.close()
  end

  def test_write_cte_rejected(visitor)
    stock = Arel.table("my_stock")
    source = Arel.from(stock).where(stock.column("qty").gt(1))
    update = Arel.update(stock).with("selected", source).set({"qty": 9}).all()
    message = nil
    begin
      update.to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support write CTEs", message)
  end

  def test_nulls_ordering_rejected(visitor)
    scores = Arel.table("my_scores")
    ordering = Arel.asc(scores.column("score")).nulls_last()
    message = nil
    begin
      Arel.from(scores).order(ordering).to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MySQL visitor does not support explicit NULL ordering", message)
  end

  def test_bitwise_operators(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_masks")
    db.execute("CREATE TABLE my_masks (value INT)")
    db.execute("INSERT INTO my_masks (value) VALUES (13)")
    masks = Arel.table("my_masks")
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

  def test_quoting_with_embedded_backtick(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_quoting")
    db.execute("CREATE TABLE my_quoting (`na``me` TEXT)")
    quoting = Arel.table("my_quoting")
    insert = Arel.insert_into(quoting).values({"na`me": "value"})
    insert.execute(db, visitor)
    rows = Arel.from(quoting).where(quoting.column("na`me").eq("value")).to_a(db, visitor)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("value", rows[0]["na`me"])
    db.close()
  end

  def test_insert_select_with_upsert(conn, visitor)
    db = open_db(conn)
    db.execute("DROP TABLE IF EXISTS my_source_items")
    db.execute("DROP TABLE IF EXISTS my_target_items")
    db.execute("CREATE TABLE my_source_items (name TEXT, qty INT)")
    # VARCHAR, not TEXT, for the same UNIQUE-key-length reason as
    # test_upsert_ignore_and_update's own comment above.
    db.execute("CREATE TABLE my_target_items (name VARCHAR(50) UNIQUE, qty INT)")
    db.execute("INSERT INTO my_source_items (name, qty) VALUES (?, ?)", ["pens", 4])
    source_items = Arel.table("my_source_items")
    target_items = Arel.table("my_target_items")
    select_source = Arel.from(source_items).project([source_items.column("name"), source_items.column("qty")])
    insert_select = Arel.insert_into(target_items).from_query(["name", "qty"], select_source)
    insert_select = insert_select.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    # Unlike the plain-VALUES upsert test above, MySQL 8's row-alias
    # syntax has no INSERT...SELECT equivalent at all (verified live --
    # every placement of `AS new_row` for this shape is a syntax error,
    # or silently aliases the wrong table), so this renders the older
    # `VALUES(col)` form instead (MySQLVisitor#render_mysql_upsert's own
    # comment on @mysql_insert_uses_row_alias covers why).
    Minitest.assert_equal(1, insert_select.execute(db, visitor))
    # Same MySQL/MariaDB affected-rows convention as the plain-VALUES
    # upsert test: the second run's ON DUPLICATE KEY UPDATE writes the
    # same qty the row already has, so this is 0, not 1 -- verified
    # directly (a value-changing duplicate reports 2, not 1, either).
    Minitest.assert_equal(0, insert_select.execute(db, visitor))
    Minitest.assert_equal(1, db.query("SELECT * FROM my_target_items").length())
    db.close()
  end

  suite = Minitest.new()
  suite.test("select/where/join/order") do
    test_select_where_join_order(conn, visitor)
  end
  suite.test("pagination incl. bare offset") do
    test_pagination(conn, visitor)
  end
  suite.test("RETURNING rejected on INSERT/UPDATE/DELETE") do
    test_returning_rejected_everywhere(conn, visitor)
  end
  suite.test("DEFAULT VALUES + per-column DEFAULT") do
    test_default_values_and_column_default(conn, visitor)
  end
  suite.test("upsert IGNORE / ON DUPLICATE KEY UPDATE (row-alias form)") do
    test_upsert_ignore_and_update(conn, visitor)
  end
  suite.test("upsert target-shape rejections") do test_upsert_target_shape_rejections(visitor) end
  suite.test("read + recursive CTEs") do
    test_read_and_recursive_cte(conn, visitor)
  end
  suite.test("write CTEs rejected") do test_write_cte_rejected(visitor) end
  suite.test("NULLS ordering rejected") do test_nulls_ordering_rejected(visitor) end
  suite.test("integer bitwise operators") do
    test_bitwise_operators(conn, visitor)
  end
  suite.test("identifier quoting with embedded backtick") do
    test_quoting_with_embedded_backtick(conn, visitor)
  end
  suite.test("INSERT SELECT with upsert") do
    test_insert_select_with_upsert(conn, visitor)
  end
  suite.run!()
end

run_tests()
