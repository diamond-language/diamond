module Registry
  # One connection per worker. Call Schema.apply before constructing this service.
  # Roots and verifier executable are trusted operator configuration.
  class Publisher
    def initialize(db, store, staging: String, verifier: String)
      unless File.directory?(staging) then raise ArgumentError.new("staging directory must exist") end
      @db = db
      @store = store
      @staging = staging
      @verifier = verifier
      @db.execute("PRAGMA foreign_keys = ON")
      @db.execute("PRAGMA synchronous = FULL")
      @db.execute("PRAGMA busy_timeout = 5000")
    end

    def publish(name: String, bytes: String, token: String, key: String = "")
      if bytes.length() == 0 || bytes.length() > 58720256
        raise ArgumentError.new("payload_too_large")
      end
      if key.length() > 128 then raise ArgumentError.new("invalid_request") end
      characters = token.chars()
      index = 0
      while index < characters.length()
        code = characters[index].ord()
        if code <= 32 || code == 127 then raise RuntimeError.new("unauthorized") end
        index += 1
      end
      digest = Digest.sha256(bytes)
      temporary = nil
      @db.execute("BEGIN IMMEDIATE")
      begin
        now = Time.now().to_i()
        credentials = @db.query("SELECT * FROM credentials WHERE token_digest = ? AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > ?)", [Digest.sha256(token), now])
        if token.length() == 0 || credentials.length() != 1 then raise RuntimeError.new("unauthorized") end
        credential = credentials[0]
        scopes = JSON.parse(credential["scopes"])
        unless scopes.include?("publish:#{name}") then raise RuntimeError.new("forbidden") end
        cuts = @db.query("SELECT * FROM cuts WHERE name = ?", [name])
        if cuts.length() > 0
          owners = @db.query("SELECT owner FROM owners WHERE cut_id = ? AND owner = ?", [cuts[0]["id"], credential["subject"]])
          if owners.length() == 0 then raise RuntimeError.new("forbidden") end
        end
        if key != ""
          keys = @db.query("SELECT body_sha256 FROM idempotency_keys WHERE credential_id = ? AND key = ?", [credential["id"], key])
          if keys.length() > 0 && keys[0]["body_sha256"] != digest
            raise RuntimeError.new("idempotency_conflict")
          end
        end
        candidate = File.join(@staging, "upload-#{SecureRandom.hex(32)}")
        File.publish(candidate, bytes)
        temporary = candidate
        verified = Process.run([@verifier, "verify", temporary, "--sha256", digest, "--json"])
        unless verified.success?() then raise ArgumentError.new("invalid_archive") end
        metadata = JSON.parse(verified.stdout())
        if metadata["protocol"] != 1 || metadata["name"] != name || metadata["sha256"] != digest || metadata["size"] != bytes.length()
          raise ArgumentError.new("invalid_archive")
        end
        File.delete(temporary)
        temporary = nil
        if cuts.length() == 0
          @db.execute("INSERT INTO cuts (name, created_at) VALUES (?, ?)", [name, now])
          cut_id = @db.last_insert_row_id()
          @db.execute("INSERT INTO owners (cut_id, owner, created_at) VALUES (?, ?, ?)", [cut_id, credential["subject"], now])
          @db.execute("INSERT INTO audit_events (subject, action, cut_name, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, 'claim', ?, ?, ?, ?, ?)", [credential["subject"], name, now, credential["id"], credential["scopes"], credential["expires_at"]])
        else
          cut_id = cuts[0]["id"]
        end
        version = metadata["version"]
        releases = @db.query("SELECT * FROM releases WHERE cut_id = ? AND version = ?", [cut_id, version])
        created = releases.length() == 0
        yanked = false
        if !created
          release = releases[0]
          if release["sha256"] != digest || release["size"] != bytes.length() || release["takedown_reason"] != nil
            raise RuntimeError.new("release_exists")
          end
          yanked = release["yanked"] == 1
        end
        # Existing complete blobs can be left by a previous rolled-back attempt.
        # Verify and synchronize them before creating any database reference.
        if @store.contains?(digest)
          unless @store.read(digest) == bytes then raise IOError.new("blob conflict") end
          File.sync(@store.path(digest))
        else
          @store.put(digest, bytes)
        end
        if created
          @db.execute("INSERT INTO releases (cut_id, version, dependencies, sha256, size, created_at) VALUES (?, ?, ?, ?, ?, ?)", [cut_id, version, JSON.stringify(metadata["dependencies"]), digest, bytes.length(), now])
          @db.execute("INSERT INTO audit_events (subject, action, cut_name, version, sha256, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, 'publish', ?, ?, ?, ?, ?, ?, ?)", [credential["subject"], name, version, digest, now, credential["id"], credential["scopes"], credential["expires_at"]])
        end
        if key != ""
          @db.execute("INSERT OR IGNORE INTO idempotency_keys (credential_id, key, body_sha256, created_at) VALUES (?, ?, ?, ?)", [credential["id"], key, digest, now])
        end
        @db.execute("COMMIT")
        {"protocol": 1, "name": name, "version": version, "sha256": digest, "size": bytes.length(), "yanked": yanked, "created": created}
      rescue error: StandardError
        @db.execute("ROLLBACK")
        if temporary != nil then File.delete(temporary) end
        raise error
      end
    end
  end
end
