require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

def run_tests()
  events = []
  def collect(event)
    events << event
  end

  raw = SQLite3.open(":memory:")
  db = ActiveRecord::InstrumentedConnection.new(raw, collect)

  db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("INSERT INTO items (name) VALUES (?)", ["safe secret"])
  Minitest.assert_equal(1, db.last_insert_row_id())
  rows = db.query("SELECT * FROM items WHERE name = ?", ["safe secret"])
  Minitest.assert_equal(1, rows.length())

  Minitest.assert_equal(6, events.length())
  Minitest.assert_equal("started", events[0]["phase"])
  Minitest.assert_equal("execute", events[0]["operation"])
  Minitest.assert_equal("completed", events[1]["phase"])
  Minitest.assert_equal(1, events[3]["affected"])
  Minitest.assert_equal("query", events[4]["operation"])
  Minitest.assert_equal(1, events[4]["bind_count"])
  Minitest.assert_equal(nil, events[4]["params"])
  Minitest.assert_equal(1, events[5]["rows"])
  Minitest.assert(events[5]["duration_ms"] >= 0)
  Minitest.assert_equal(events[4]["query_id"], events[5]["query_id"])

  failed = false
  begin
    db.query("SELECT * FROM missing_table")
  rescue error: SQLite3Error
    failed = true
  end
  Minitest.assert_equal(true, failed)
  Minitest.assert_equal("started", events[6]["phase"])
  Minitest.assert_equal("failed", events[7]["phase"])
  Minitest.assert(events[7]["duration_ms"] >= 0)
  Minitest.assert(events[7]["error"].include?("missing_table"))

  db.close()
end

run_tests()
puts("active_record instrumented connection smoke ok")
