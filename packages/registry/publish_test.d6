require_cut "registry"

def check(value, message)
  unless value then raise message end
end

def rejected(publisher, name, bytes, token, key, expected)
  caught = false
  begin
    publisher.publish(name, bytes, token, key)
  rescue error: StandardError
    check(error.message() == expected, "unexpected error: #{error.message()}")
    caught = true
  end
  check(caught, "request was accepted: #{expected}")
end

def read_bytes(path)
  file = File.open(path, "r")
  bytes = file.read()
  file.close()
  bytes
end

root = ENV["REGISTRY_PUBLISH_ROOT"]
db = SQLite3.open(File.join(root, "registry.db"))
Registry::Schema.apply(db)
store = Registry::BlobStore.new(File.join(root, "blobs"))
publisher = Registry::Publisher.new(db, store, File.join(root, "staging"), ENV["REGISTRY_FACET"])
bytes = read_bytes(File.join(root, "first.tar"))
changed = read_bytes(File.join(root, "changed.tar"))
token = SecureRandom.hex(32)
db.execute("INSERT INTO credentials (token_digest, subject, scopes, created_at) VALUES (?, 'alice', ?, 0)", [Digest.sha256(token), JSON.stringify(["publish:publish_test", "publish:wrong_name"])])
rejected(publisher, "publish_test", bytes, "wrong", "", "unauthorized")
rejected(publisher, "other", bytes, token, "", "forbidden")
rejected(publisher, "wrong_name", bytes, token, "", "invalid_archive")
rejected(publisher, "publish_test", "invalid tar", token, "", "invalid_archive")
check(db.query("SELECT * FROM cuts").length() == 0, "invalid requests claimed names")
check(Dir.entries(File.join(root, "staging")).length() == 0, "failed upload leaked staging file")

# Fail after storing the blob: all metadata and ownership must roll back.
db.execute("CREATE TRIGGER reject_audit BEFORE INSERT ON audit_events WHEN NEW.action = 'publish' BEGIN SELECT RAISE(ABORT, 'injected audit failure'); END")
failed = false
begin
  publisher.publish("publish_test", bytes, token, "release-key")
rescue error: StandardError
  failed = true
end
check(failed, "audit failure was ignored")
check(db.query("SELECT * FROM cuts").length() == 0, "claim survived rollback")
check(db.query("SELECT * FROM releases").length() == 0, "release survived rollback")
check(db.query("SELECT * FROM audit_events").length() == 0, "audit survived rollback")
check(db.query("SELECT * FROM idempotency_keys").length() == 0, "key survived rollback")
check(store.read(Digest.sha256(bytes)) == bytes, "expected complete orphan blob")
db.execute("DROP TRIGGER reject_audit")
result = publisher.publish("publish_test", bytes, token, "release-key")
check(result["created"], "orphan recovery did not create release")
check(!publisher.publish("publish_test", bytes, token, "release-key")["created"], "retry created another release")
check(db.query("SELECT * FROM audit_events WHERE action = 'publish'").length() == 1, "retry duplicated audit")
audit = db.query("SELECT * FROM audit_events WHERE action = 'publish'")[0]
check(audit["credential_id"] != nil, "audit omitted credential identity")
check(JSON.parse(audit["credential_scopes"]).include?("publish:publish_test"), "audit omitted scopes")
row = db.query("SELECT * FROM releases")[0]
check(JSON.parse(row["dependencies"])["logger"] == "^0.1.0", "archive dependency metadata lost")
rejected(publisher, "publish_test", changed, token, "release-key", "idempotency_conflict")
rejected(publisher, "publish_test", changed, token, "", "release_exists")
db.execute("UPDATE releases SET yanked = 1")
check(publisher.publish("publish_test", bytes, token)["yanked"], "retry unyanked a release")
db.execute("UPDATE releases SET takedown_reason = 'operator removal'")
rejected(publisher, "publish_test", bytes, token, "", "release_exists")
db.execute("UPDATE credentials SET expires_at = 1")
rejected(publisher, "publish_test", bytes, token, "", "unauthorized")
db.execute("UPDATE credentials SET expires_at = NULL, revoked_at = 1")
rejected(publisher, "publish_test", bytes, token, "", "unauthorized")
db.execute("UPDATE credentials SET revoked_at = NULL, subject = 'bob'")
rejected(publisher, "publish_test", bytes, token, "", "forbidden")
db.close()
db = SQLite3.open(File.join(root, "registry.db"))
check(db.query("SELECT * FROM releases").length() == 1, "release did not persist")
db.close()
puts("registry publish transaction tests passed")
