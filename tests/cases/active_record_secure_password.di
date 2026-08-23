require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

# has_secure_password-style helper, wired up the same explicit way every
# other association/helper in this package is (see model.di's own
# comment): an ordinary attr_accessor password_digest, plus
# #secure_password=/#authenticate from Model.
class User < ActiveRecord::Model
  # password_digest deliberately untyped, not `: String` -- a model that
  # never called #secure_password= (an invited-but-not-yet-onboarded user,
  # say) has a nil digest, and a typed attr_accessor's generated getter
  # enforces its return type at runtime, raising TypeError on a nil read
  # rather than just returning nil.
  attr_accessor email: String, password_digest

  def initialize(attributes: Hash = {})
    super(attributes)
    @email = attributes["email"]
    @password_digest = attributes["password_digest"]
  end

  def to_attributes() = {"email": @email, "password_digest": @password_digest}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

def build_user(row) = User.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT, password_digest TEXT)")
  User.configure(ActiveRecord::Repository.new(Arel.table("users"), build_user, "id"))

  user = User.new({"email": "ada@example.com"})
  user.secure_password = "hunter2"
  Minitest.assert(user.password_digest() != nil)
  Minitest.assert(user.password_digest() != "hunter2")
  Minitest.assert(user.password_digest().slice(0, 4) == "$2b$")

  user.save(db)
  reloaded = User.find(db, 1)
  Minitest.assert_equal(true, reloaded.authenticate("hunter2"))
  Minitest.assert_equal(false, reloaded.authenticate("wrong password"))

  # A model that never called #secure_password= has a nil digest --
  # #authenticate must not raise on that, just always report no match.
  nobody = User.new({"email": "nobody@example.com"})
  Minitest.assert_equal(false, nobody.authenticate("anything"))

  db.close()
end

run_tests()
puts("active_record secure_password smoke ok")
