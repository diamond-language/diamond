module Registry
  class Schema
    def self.version() = "2026092201"

    # The registry keeps release metadata in SQLite and archive bytes in the
    # BlobStore. Every release points at one immutable digest; yanking changes
    # visibility without changing that digest.
    def self.apply(db)
      db.execute("CREATE TABLE IF NOT EXISTS schema_migrations (version TEXT PRIMARY KEY NOT NULL)")
      applied = db.query("SELECT version FROM schema_migrations")
      already_applied = applied.any?() do |row|
        row["version"] == Schema.version()
      end
      unless already_applied
        ActiveRecord::Transaction.run(db) do
          db.execute("CREATE TABLE cuts (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL)")
          db.execute("CREATE TABLE releases (id INTEGER PRIMARY KEY, cut_id INTEGER NOT NULL, version TEXT NOT NULL, dependencies TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL, yanked INTEGER NOT NULL DEFAULT 0, takedown_reason TEXT, created_at INTEGER NOT NULL, FOREIGN KEY(cut_id) REFERENCES cuts(id), UNIQUE(cut_id, version), UNIQUE(sha256))")
          db.execute("CREATE TABLE owners (cut_id INTEGER NOT NULL, owner TEXT NOT NULL, created_at INTEGER NOT NULL, PRIMARY KEY(cut_id, owner), FOREIGN KEY(cut_id) REFERENCES cuts(id))")
          db.execute("CREATE TABLE credentials (id INTEGER PRIMARY KEY, token_digest TEXT NOT NULL UNIQUE, subject TEXT NOT NULL, scopes TEXT NOT NULL, expires_at INTEGER, revoked_at INTEGER, created_at INTEGER NOT NULL)")
          db.execute("CREATE TABLE audit_events (id INTEGER PRIMARY KEY, subject TEXT NOT NULL, action TEXT NOT NULL, cut_name TEXT, version TEXT, sha256 TEXT, reason TEXT, created_at INTEGER NOT NULL)")
          db.execute("CREATE TABLE idempotency_keys (credential_id INTEGER NOT NULL, key TEXT NOT NULL, body_sha256 TEXT NOT NULL, created_at INTEGER NOT NULL, PRIMARY KEY(credential_id, key), FOREIGN KEY(credential_id) REFERENCES credentials(id))")
          db.execute("INSERT INTO schema_migrations (version) VALUES (?)", [Schema.version()])
        end
      end
      # Add credential attribution without recreating an existing registry.
      audit_version = "2026092202"
      recorded = db.query("SELECT version FROM schema_migrations WHERE version = ?", [audit_version])
      if recorded.length() == 0
        ActiveRecord::Transaction.run(db) do
          db.execute("ALTER TABLE audit_events ADD COLUMN credential_id INTEGER REFERENCES credentials(id)")
          db.execute("ALTER TABLE audit_events ADD COLUMN credential_scopes TEXT")
          db.execute("ALTER TABLE audit_events ADD COLUMN credential_expires_at INTEGER")
          db.execute("INSERT INTO schema_migrations (version) VALUES (?)", [audit_version])
        end
      end
    end
  end
end
