module Registry
  class API
    def initialize(db, store, publisher, base: String = "")
      @db = db
      @store = store
      @publisher = publisher
      @base = base
      @administration = Administration.new(db)
    end

    def json(status, body)
      [status, {"Content-Type": "application/json", "Cache-Control": "no-store"}, JSON.stringify(body)]
    end

    def error(status, code)
      response = self.json(status, {"protocol": 1, "error": code, "message": code, "request_id": SecureRandom.hex(16)})
      if status == 401 then response[1]["WWW-Authenticate"] = "Bearer" end
      response
    end

    def numeric?(text)
      if text.length() == 0 then return false end
      chars = text.chars()
      index = 0
      while index < chars.length()
        code = chars[index].ord()
        if code < 48 || code > 57 then return false end
        index += 1
      end
      true
    end

    # SemVer precedence for the canonical versions emitted by facet verify.
    def before?(left, right)
      left_parts = left.split("-")
      right_parts = right.split("-")
      a = left_parts[0].split(".")
      b = right_parts[0].split(".")
      i = 0
      while i < 3
        if a[i].to_i() != b[i].to_i() then return a[i].to_i() < b[i].to_i() end
        i += 1
      end
      if left_parts.length() == 1 then return false end
      if right_parts.length() == 1 then return true end
      a = left.slice(left_parts[0].length() + 1, left.length()).split(".")
      b = right.slice(right_parts[0].length() + 1, right.length()).split(".")
      i = 0
      while i < a.length() && i < b.length()
        if a[i] != b[i]
          an = self.numeric?(a[i])
          bn = self.numeric?(b[i])
          if an && bn then return a[i].to_i() < b[i].to_i() end
          if an != bn then return an end
          return a[i] < b[i]
        end
        i += 1
      end
      a.length() < b.length()
    end

    def index(name)
      cuts = @db.query("SELECT id FROM cuts WHERE name = ?", [name])
      if cuts.length() == 0 then return self.error(404, "not_found") end
      rows = @db.query("SELECT version, yanked FROM releases WHERE cut_id = ? AND takedown_reason IS NULL", [cuts[0]["id"]])
      versions = []
      rows.each() do |row|
        versions.push({"version": row["version"], "yanked": row["yanked"] == 1})
        i = versions.length() - 1
        while i > 0 && self.before?(versions[i]["version"], versions[i - 1]["version"])
          previous = versions[i - 1]
          versions[i - 1] = versions[i]
          versions[i] = previous
          i -= 1
        end
      end
      self.json(200, {"protocol": 1, "versions": versions})
    end

    def release(name, version)
      rows = @db.query("SELECT releases.* FROM releases JOIN cuts ON cuts.id = releases.cut_id WHERE cuts.name = ? AND version = ? AND takedown_reason IS NULL", [name, version])
      if rows.length() == 0 then return self.error(404, "not_found") end
      row = rows[0]
      self.json(200, {"protocol": 1, "name": name, "version": version, "dependencies": JSON.parse(row["dependencies"]), "yanked": row["yanked"] == 1,
        "archive": {"path": "/v1/blobs/sha256/#{row["sha256"]}", "sha256": row["sha256"], "size": row["size"]}})
    end

    def blob(digest)
      BlobStore.validate_digest(digest)
      rows = @db.query("SELECT size FROM releases WHERE sha256 = ? AND takedown_reason IS NULL", [digest])
      if rows.length() == 0 then return self.error(404, "not_found") end
      bytes = @store.read(digest)
      if bytes.length() != rows[0]["size"] then raise IOError.new("blob size mismatch") end
      [200, {"Content-Type": "application/octet-stream", "Cache-Control": "no-store"}, bytes]
    end

    def upload(name, request)
      headers = request["headers"]
      authorization = headers["authorization"]
      if authorization == nil || !authorization.start_with?("Bearer ") then return self.error(401, "unauthorized") end
      if headers["content-type"] != "application/octet-stream" then return self.error(400, "invalid_request") end
      key = headers["idempotency-key"]
      if key == nil then key = "" end
      result = @publisher.publish(name, request["body"], authorization.slice(7, authorization.length() - 7), key)
      status = if result["created"] then 201 else 200 end
      self.json(status, {"protocol": 1, "name": result["name"], "version": result["version"], "sha256": result["sha256"], "size": result["size"], "yanked": result["yanked"]})
    end

    def manage(name, version, action, request)
      authorization = request["headers"]["authorization"]
      if authorization == nil || !authorization.start_with?("Bearer ") then return self.error(401, "unauthorized") end
      if request["headers"]["content-type"] != "application/json" || request["body"].length() > 4096
        return self.error(400, "invalid_request")
      end
      begin
        payload = JSON.parse(request["body"])
      rescue error: StandardError
        return self.error(400, "invalid_request")
      end
      unless payload is Hash then return self.error(400, "invalid_request") end
      unless payload["reason"] is String then return self.error(400, "invalid_request") end
      if payload.length() != 1 then return self.error(400, "invalid_request") end
      result = @administration.change(name, version, action, payload["reason"], authorization.slice(7, authorization.length() - 7))
      if action == "takedown" then return self.json(200, result) end
      self.release(name, version)
    end

    def bearer(request)
      authorization = request["headers"]["authorization"]
      if authorization == nil || !authorization.start_with?("Bearer ") then raise RuntimeError.new("unauthorized") end
      authorization.slice(7, authorization.length() - 7)
    end

    def owner_request(name, action, request)
      token = self.bearer(request)
      if action == "list" then return self.json(200, @administration.owners(name, token)) end
      if request["headers"]["content-type"] != "application/json" || request["body"].length() > 4096 then raise ArgumentError.new("invalid_request") end
      begin
        payload = JSON.parse(request["body"])
      rescue error: StandardError
        raise ArgumentError.new("invalid_request")
      end
      unless payload is Hash then raise ArgumentError.new("invalid_request") end
      unless payload["owner"] is String then raise ArgumentError.new("invalid_request") end
      unless payload["reason"] is String then raise ArgumentError.new("invalid_request") end
      if payload.length() != 2 then raise ArgumentError.new("invalid_request") end
      self.json(200, @administration.change_owner(name, payload["owner"], action, payload["reason"], token))
    end

    def audit_request(path, request)
      token = self.bearer(request)
      parameters = {"after": 0, "limit": 50}
      parts = path.split("?")
      if parts.length() > 2 then raise ArgumentError.new("invalid_request") end
      if parts.length() == 2
        seen = []
        parts[1].split("&").each() do |pair|
          values = pair.split("=")
          if values.length() != 2 then raise ArgumentError.new("invalid_request") end
          key = values[0]
          if !["after", "limit"].include?(key) || seen.include?(key) then raise ArgumentError.new("invalid_request") end
          seen.push(key)
          value = values[1].to_i()
          if "#{value}" != values[1] || value > 9223372036854775807 then raise ArgumentError.new("invalid_request") end
          parameters[key] = value
        end
      end
      self.json(200, @administration.audit(token, parameters["after"], parameters["limit"]))
    end

    def call(request)
      begin
        path = request["path"]
        if @base != ""
          unless path.start_with?("#{@base}/") then return self.error(404, "not_found") end
          path = path.slice(@base.length(), path.length() - @base.length())
        end
        method = request["method"]
        if path == "/health" && method == "GET"
          @db.query("SELECT 1")
          return self.json(200, {"protocol": 1, "status": "ok"})
        end
        if method == "GET" && (path == "/v1/audit" || path.start_with?("/v1/audit?"))
          return self.audit_request(path, request)
        end
        parts = path.split("/")
        if parts.length() >= 5 && parts[1] == "v1" && parts[2] == "cuts" && parts[4] == "owners"
          if parts.length() == 5 && method == "GET" then return self.owner_request(parts[3], "list", request) end
          if parts.length() == 6 && method == "POST" && ["add", "remove"].include?(parts[5])
            return self.owner_request(parts[3], parts[5], request)
          end
        end
        if parts.length() == 5 && parts[1] == "v1" && parts[2] == "blobs" && parts[3] == "sha256" && method == "GET"
          return self.blob(parts[4])
        end
        if parts.length() >= 5 && parts[1] == "v1" && parts[2] == "cuts" && parts[4] == "versions"
          if parts.length() == 5
            if method == "GET" then return self.index(parts[3]) end
            if method == "POST" then return self.upload(parts[3], request) end
          elsif parts.length() == 7 && method == "POST" && ["yank", "unyank", "takedown"].include?(parts[6])
            return self.manage(parts[3], parts[5], parts[6], request)
          elsif parts.length() == 6 && method == "GET"
            return self.release(parts[3], parts[5])
          end
        end
        self.error(404, "not_found")
      rescue error: ArgumentError
        code = error.message()
        if code == "payload_too_large" then return self.error(413, code) end
        if code == "invalid_archive" then return self.error(422, code) end
        self.error(400, "invalid_request")
      rescue error: RuntimeError
        code = error.message()
        if code == "unauthorized" then return self.error(401, code) end
        if code == "forbidden" then return self.error(403, code) end
        if code == "not_found" then return self.error(404, code) end
        if code == "release_exists" || code == "idempotency_conflict" || code == "last_owner" then return self.error(409, code) end
        self.error(500, "internal_error")
      rescue error: StandardError
        self.error(500, "internal_error")
      end
    end
  end
end
