# hash_value/values_equal's DIAMOND_VALUE_OBJECT switch only special-
# cased Instance/Array/Hash (identity), Symbol (content), Time (by
# epoch value), and Bignum (handled earlier) -- every other native
# object kind (Closure, Fiber, File, Listener, Socket, UdpSocket,
# TlsSocket, Regexp, ProgramBuilder, Thread, SQLite3, SQLite3
# Statement, Postgres, MySQL, ProcessResult) unconditionally fell
# through to a final default that read String fields (chars/length)
# off the object's underlying struct -- garbage for anything that
# isn't actually a String, and a genuine segfault when actually
# exercised (confirmed live: `cache[SQLite3.open(":memory:")] = x`
# crashed the whole process before this fix). Found while spiking a
# statement-cache design that needed to key a Hash by a connection
# object. Fixed by giving every otherwise-unhandled object kind
# identity equality/hashing, the same rule Instance/Array/Hash already
# use, instead of falling through to the String-cast default.
require "../../lib/minitest"

def compute_for_thread_identity_test()
  1 + 1
end

def run_tests()
  def test_distinct_connections_hash_and_compare_by_identity()
    a = SQLite3.open(":memory:")
    b = SQLite3.open(":memory:")
    cache = {}
    cache[a] = "value-for-a"
    Minitest.assert_equal("value-for-a", cache[a])
    Minitest.assert_equal(nil, cache[b])
    Minitest.assert(cache.include_key?(a))
    Minitest.refute(cache.include_key?(b))
    Minitest.assert(a == a)
    Minitest.refute(a == b)
    Minitest.assert(a != b)
    a.close()
    b.close()
  end

  def test_regexp_and_thread_also_hash_and_compare_by_identity()
    re1 = Regexp.new("a+")
    re2 = Regexp.new("a+")
    cache = {}
    cache[re1] = "first"
    Minitest.assert_equal("first", cache[re1])
    Minitest.assert_equal(nil, cache[re2])
    Minitest.assert(re1 == re1)
    Minitest.refute(re1 == re2)

    thread = Thread.new(compute_for_thread_identity_test)
    thread.join()
    Minitest.assert(thread == thread)
  end

  suite = Minitest.new()
  suite.test("distinct native objects (SQLite3 connections) hash/compare by identity, not crash",
             test_distinct_connections_hash_and_compare_by_identity)
  suite.test("other native object kinds (Regexp, Thread) also hash/compare by identity",
             test_regexp_and_thread_also_hash_and_compare_by_identity)
  suite.run!()
end

run_tests()
