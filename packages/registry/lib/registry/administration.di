module Registry
  class Administration
    def initialize(db)
      @db = db
      @db.execute("PRAGMA foreign_keys = ON")
      @db.execute("PRAGMA synchronous = FULL")
      @db.execute("PRAGMA busy_timeout = 5000")
    end

    def reason!(reason: String)
      if reason.strip().length() == 0 || reason.length() > 1024
        raise ArgumentError.new("invalid_request")
      end
    end

    def authenticate(token: String, scope: String)
      if token.length() == 0 then raise RuntimeError.new("unauthorized") end
      rows = @db.query("SELECT * FROM credentials WHERE token_digest = ? AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > ?)", [Digest.sha256(token), Time.now().to_i()])
      if rows.length() != 1 then raise RuntimeError.new("unauthorized") end
      credential = rows[0]
      unless JSON.parse(credential["scopes"]).include?(scope) then raise RuntimeError.new("forbidden") end
      credential
    end

    def change(name: String, version: String, action: String, reason: String, token: String)
      self.reason!(reason)
      unless ["yank", "unyank", "takedown"].include?(action) then raise ArgumentError.new("invalid_request") end
      @db.execute("BEGIN IMMEDIATE")
      begin
        scope = if action == "takedown" then "admin" else "manage:#{name}" end
        credential = self.authenticate(token, scope)
        rows = @db.query("SELECT releases.* FROM releases JOIN cuts ON cuts.id = releases.cut_id WHERE cuts.name = ? AND version = ?", [name, version])
        if action != "takedown"
          owners = @db.query("SELECT owners.owner FROM owners JOIN cuts ON cuts.id = owners.cut_id WHERE cuts.name = ? AND owners.owner = ?", [name, credential["subject"]])
          if owners.length() == 0 then raise RuntimeError.new("forbidden") end
        end
        if rows.length() == 0 then raise RuntimeError.new("not_found") end
        row = rows[0]
        changed = false
        if action == "takedown"
          if row["takedown_reason"] == nil
            @db.execute("UPDATE releases SET takedown_reason = ? WHERE id = ?", [reason, row["id"]])
            changed = true
          end
        else
          if row["takedown_reason"] != nil then raise RuntimeError.new("not_found") end
          desired = if action == "yank" then 1 else 0 end
          if row["yanked"] != desired
            @db.execute("UPDATE releases SET yanked = ? WHERE id = ?", [desired, row["id"]])
            changed = true
          end
        end
        if changed
          @db.execute("INSERT INTO audit_events (subject, action, cut_name, version, sha256, reason, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", [credential["subject"], action, name, version, row["sha256"], reason, Time.now().to_i(), credential["id"], credential["scopes"], credential["expires_at"]])
        end
        @db.execute("COMMIT")
        {"protocol": 1, "name": name, "version": version, "taken_down": action == "takedown"}
      rescue error: StandardError
        @db.execute("ROLLBACK")
        raise error
      end
    end

    def validate_credential(subject: String, scopes: Array, expires: Int, reason: String, operator: String)
      self.reason!(reason)
      if subject.strip().length() == 0 || subject.length() > 256 || operator.strip().length() == 0 || operator.length() > 256 || scopes.length() == 0 || expires <= Time.now().to_i()
        raise ArgumentError.new("invalid_request")
      end
      scopes.each() do |scope|
        unless scope is String then raise ArgumentError.new("invalid_request") end
        if scope != "admin"
          parts = scope.split(":")
          if parts.length() != 2 || !["publish", "manage"].include?(parts[0])
            raise ArgumentError.new("invalid_request")
          end
          name = parts[1]
          if name.length() == 0 || name.length() > 63 then raise ArgumentError.new("invalid_request") end
          chars = name.chars()
          index = 0
          while index < chars.length()
            code = chars[index].ord()
            letter = code >= 97 && code <= 122
            if !letter && (index == 0 || !((code >= 48 && code <= 57) || code == 95))
              raise ArgumentError.new("invalid_request")
            end
            index += 1
          end
        end
      end
    end

    # Called only by the local operator CLI, under filesystem access control.
    def issue(subject: String, scopes: Array, expires: Int, reason: String, operator: String, previous: Int = 0)
      self.validate_credential(subject, scopes, expires, reason, operator)
      token = SecureRandom.hex(32)
      @db.execute("BEGIN IMMEDIATE")
      begin
        if previous != 0
          rows = @db.query("SELECT * FROM credentials WHERE id = ? AND revoked_at IS NULL", [previous])
          if rows.length() != 1 then raise RuntimeError.new("not_found") end
          if rows[0]["subject"] != subject || rows[0]["scopes"] != JSON.stringify(scopes)
            raise ArgumentError.new("invalid_request")
          end
          @db.execute("UPDATE credentials SET revoked_at = ? WHERE id = ?", [Time.now().to_i(), previous])
          @db.execute("INSERT INTO audit_events (subject, action, reason, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, 'credential_revoke', ?, ?, ?, ?, ?)", [operator, reason, Time.now().to_i(), previous, rows[0]["scopes"], rows[0]["expires_at"]])
        end
        @db.execute("INSERT INTO credentials (token_digest, subject, scopes, expires_at, created_at) VALUES (?, ?, ?, ?, ?)", [Digest.sha256(token), subject, JSON.stringify(scopes), expires, Time.now().to_i()])
        id = @db.last_insert_row_id()
        @db.execute("INSERT INTO audit_events (subject, action, reason, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, 'credential_issue', ?, ?, ?, ?, ?)", [operator, reason, Time.now().to_i(), id, JSON.stringify(scopes), expires])
        @db.execute("COMMIT")
        {"id": id, "token": token, "subject": subject, "scopes": scopes, "expires_at": expires}
      rescue error: StandardError
        @db.execute("ROLLBACK")
        raise error
      end
    end

    def revoke(id: Int, reason: String, operator: String)
      self.reason!(reason)
      self.reason!(operator)
      if operator.length() > 256 then raise ArgumentError.new("invalid_request") end
      @db.execute("BEGIN IMMEDIATE")
      begin
        rows = @db.query("SELECT * FROM credentials WHERE id = ?", [id])
        if rows.length() != 1 then raise RuntimeError.new("not_found") end
        row = rows[0]
        if row["revoked_at"] == nil
          @db.execute("UPDATE credentials SET revoked_at = ? WHERE id = ?", [Time.now().to_i(), id])
          @db.execute("INSERT INTO audit_events (subject, action, reason, created_at, credential_id, credential_scopes, credential_expires_at) VALUES (?, 'credential_revoke', ?, ?, ?, ?, ?)", [operator, reason, Time.now().to_i(), id, row["scopes"], row["expires_at"]])
        end
        @db.execute("COMMIT")
        {"id": id, "revoked": true}
      rescue error: StandardError
        @db.execute("ROLLBACK")
        raise error
      end
    end
  end
end
