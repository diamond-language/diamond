require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_update_accepts_a_cte()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (id INTEGER, qty INTEGER)")
    db.execute("INSERT INTO items VALUES (1, 1), (2, 4)")
    items = Arel.table("items")
    source = Arel.from(items).project(items.column("id"))
    source = source.where(items.column("qty").lt(3))
    selected = Arel.table("selected")
    ids = Arel.from(selected).project(selected.column("id"))
    update = Arel.update(items).with("selected", source).set({"qty": 9})
    update = update.where(items.column("id").in_subquery(ids))
    sql, params = update.to_sql()
    Minitest.assert_equal("WITH \"selected\" AS (SELECT \"items\".\"id\" FROM \"items\" WHERE \"items\".\"qty\" < ?) UPDATE \"items\" SET \"qty\" = ? WHERE \"items\".\"id\" IN (SELECT \"selected\".\"id\" FROM \"selected\")", sql)
    Minitest.assert_equal(3, params[0])
    Minitest.assert_equal(9, params[1])
    Minitest.assert_equal(1, update.execute(db))
    Minitest.assert_equal(9, db.query("SELECT qty FROM items WHERE id = 1")[0]["qty"])
    db.close()
  end

  def test_delete_accepts_a_cte()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE events (id INTEGER, stale INTEGER)")
    db.execute("INSERT INTO events VALUES (1, 1), (2, 0)")
    events = Arel.table("events")
    source = Arel.from(events).project(events.column("id"))
    source = source.where(events.column("stale").eq(1))
    expired = Arel.table("expired")
    ids = Arel.from(expired).project(expired.column("id"))
    deletion = Arel.delete_from(events).with("expired", source)
    deletion = deletion.where(events.column("id").in_subquery(ids))
    sql, params = deletion.to_sql()
    Minitest.assert_equal("WITH \"expired\" AS (SELECT \"events\".\"id\" FROM \"events\" WHERE \"events\".\"stale\" = ?) DELETE FROM \"events\" WHERE \"events\".\"id\" IN (SELECT \"expired\".\"id\" FROM \"expired\")", sql)
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(1, deletion.execute(db))
    Minitest.assert_equal(2, db.query("SELECT id FROM events")[0]["id"])
    db.close()
  end

  def test_write_managers_reject_duplicate_cte_names()
    items = Arel.table("items")
    source = Arel.from(items)
    messages = []
    begin
      Arel.insert_into(items).with("source", source).with("source", source)
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.update(items).with("source", source).with("source", source)
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.delete_from(items).with("source", source).with("source", source)
    rescue error: ArgumentError
      messages.push(error.message())
    end
    Minitest.assert_equal(3, messages.length())
    Minitest.assert_equal("duplicate CTE name", messages[0])
    Minitest.assert_equal("duplicate CTE name", messages[1])
    Minitest.assert_equal("duplicate CTE name", messages[2])
  end

  def test_recursive_cte_can_feed_an_insert()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE seeds (value INTEGER)")
    db.execute("CREATE TABLE results (value INTEGER)")
    db.execute("INSERT INTO seeds VALUES (1)")
    seeds = Arel.table("seeds")
    numbers = Arel.table("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    step = step.where(numbers.column("value").lt(3))
    body = Arel.union_all(anchor, step)
    selection = Arel.from(numbers).project(numbers.column("value"))
    results = Arel.table("results")
    insert = Arel.insert_into(results).with_recursive("numbers", body)
    insert = insert.from_query(["value"], selection)
    sql, params = insert.to_sql()
    Minitest.assert_equal("WITH RECURSIVE \"numbers\" AS (SELECT \"seeds\".\"value\" FROM \"seeds\" UNION ALL SELECT value + 1 FROM \"numbers\" WHERE \"numbers\".\"value\" < ?) INSERT INTO \"results\" (\"value\") SELECT \"numbers\".\"value\" FROM \"numbers\"", sql)
    insert.execute(db)
    Minitest.assert_equal(3, db.query("SELECT value FROM results").length())
    db.close()
  end

  def test_update_and_delete_accept_recursive_ctes()
    seeds = Arel.table("seeds")
    numbers = Arel.table("numbers")
    items = Arel.table("items")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    step = step.where(numbers.column("value").lt(3))
    body = Arel.union_all(anchor, step)
    ids = Arel.from(numbers).project(numbers.column("value"))
    predicate = items.column("id").in_subquery(ids)
    update = Arel.update(items).with_recursive("numbers", body)
    update = update.set({"active": true}).where(predicate)
    update_sql, update_params = update.to_sql()
    deletion = Arel.delete_from(items).with_recursive("numbers", body)
    delete_sql, delete_params = deletion.where(predicate).to_sql()
    Minitest.assert_equal(true, update_sql.start_with?("WITH RECURSIVE \"numbers\" AS"))
    Minitest.assert_equal(true, update_sql.include?(" UPDATE \"items\" SET"))
    Minitest.assert_equal(true, delete_sql.start_with?("WITH RECURSIVE \"numbers\" AS"))
    Minitest.assert_equal(true, delete_sql.include?(" DELETE FROM \"items\" WHERE"))
    Minitest.assert_equal(3, update_params[0])
    Minitest.assert_equal(3, delete_params[0])
  end

  suite = Minitest.new()
  suite.test("UPDATE CTE", test_update_accepts_a_cte)
  suite.test("DELETE CTE", test_delete_accepts_a_cte)
  suite.test("write CTE duplicate names", test_write_managers_reject_duplicate_cte_names)
  suite.test("recursive INSERT CTE", test_recursive_cte_can_feed_an_insert)
  suite.test("recursive UPDATE and DELETE CTEs", test_update_and_delete_accept_recursive_ctes)
  suite.run!()
end

run_tests()
