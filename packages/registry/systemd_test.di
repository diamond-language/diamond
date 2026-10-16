# Privileged staging drill: run only in a disposable local QEMU guest.
#
#   REGISTRY_QEMU_STAGING=1 diamond packages/registry/systemd_test.di        (as root)
require "../../tests/harness"

root = File.dirname(File.dirname(__dir__))

def run(argv, env, cwd)
  result = Harness.run(argv.map() do |part| part.to_s() end, env, cwd)
  raise "#{File.basename(argv[0].to_s())} failed: #{result.stderr()}" unless result.success?()
  result.stdout()
end

def check(condition, message)
  raise message unless condition
end

# The owner (numeric uid) of a path, through stat(1).
def owner_uid(path)
  run(["stat", "-c", "%u", path], nil, nil).strip().to_i()
end

class Drill
  def initialize(root: String)
    @root = root
    @work = Harness.make_temp_dir() # replaced below: the drill lives under /opt
    @name = ""
    @unit_name = ""
    @unit = ""
    @state = ""
    @environment = ""
    @app = ""
    @port = 0
    @uid = 0
    @account_created = false
    @unit_created = false
    @token = nil
    @payload = nil
    @headers = nil
    @digest = nil
    @cut = nil
  end

  def prepare()
    Harness.remove_tree(@work)
    base = Process.run(["mktemp", "-d", "/opt/diamond-registry-systemd-XXXXXXXX"]).stdout().strip()
    @work = base
    run(["chmod", "755", @work], nil, nil)
    @name = "dreg-" + File.basename(@work).split("-")[-1]
    @unit_name = @name + ".service"
    @unit = "/run/systemd/system/" + @unit_name
    @state = "/var/lib/" + @name
    @environment = @work + "/registry.env"
    check(!File.exist?(@unit) && !File.exist?(@state), "the unit or state directory already exists")
  end

  def journal_tail()
    Harness.run(["journalctl", "-u", @unit_name, "--no-pager", "-n", "80"], nil, nil).stdout()
  end

  def cleanup()
    if @unit_created
      Harness.run(["systemctl", "stop", @unit_name], nil, nil)
      File.delete(@unit)
      Harness.run(["systemctl", "daemon-reload"], nil, nil)
      Harness.run(["systemctl", "reset-failed", @unit_name], nil, nil)
    end
    if @account_created
      Harness.run(["rm", "-rf", @state], nil, nil)
      run(["userdel", @name], nil, nil)
    end
    Harness.run(["rm", "-rf", @work], nil, nil)
  end

  def write_environment(data: String)
    File.write(@environment, "REGISTRY_ROOT=#{data}\nREGISTRY_FACET=#{@work}/facet\nREGISTRY_PORT=#{@port}\n" +
      "REGISTRY_BASE=/registry\nDIAMOND_NO_CACHE=1\n")
    run(["chmod", "600", @environment], nil, nil)
  end

  def property_value(key: String)
    run(["systemctl", "show", @unit_name, "--property=" + key, "--value"], nil, nil).strip()
  end

  def request(path: String, method: String, body, headers)
    answer = Harness.http_with_headers(@port, method, "/registry" + path, body, headers)
    answer
  end

  def healthy(previous)
    pid = 0
    Harness.wait_for(30000, "service did not become healthy") do
      ok = false
      begin
        pid = property_value("MainPID").to_i()
        ok = pid != 0 && pid != previous && self.request("/health", "GET", nil, {})[0] == 200
      rescue error: IOError
        ok = false
      end
      Harness.sleep_ms(200) unless ok
      ok
    end
    pid
  end

  def install()
    run(["useradd", "--system", "--no-create-home", "--shell", "/usr/sbin/nologin", @name], nil, nil)
    @account_created = true
    @uid = run(["id", "-u", @name], nil, nil).strip().to_i()
    @app = @work + "/app"
    Dir.mkdir(@app, 0o755)
    run([@root + "/tools/install_local_cuts.sh", @app], nil, nil)
    ["app.di", "credentials.di", "backup.di", "catalog.di", "catalog.html", "catalog.js", "catalog.css", "cut.html", "cut.js", "markdown.js"].each() do |filename|
      File.copy(@root + "/applications/registry/" + filename, @app + "/" + filename)
    end
    ["diamond", "facet"].each() do |executable|
      File.copy(@root + "/build/" + executable, @work + "/" + executable)
    end
    # Root-owned code is readable, but not writable, by the service account.
    run(["find", @app, "-type", "d", "-exec", "chmod", "755", "{}", "+"], nil, nil)
    run(["find", @app, "-type", "f", "-exec", "chmod", "644", "{}", "+"], nil, nil)
    Dir.mkdir(@state, 0o700)
    run(["chown", "#{@uid}:#{@uid}", @state], nil, nil)
    ["blobs", "staging"].each() do |directory|
      Dir.mkdir(@state + "/" + directory, 0o700)
      run(["chown", "#{@uid}:#{@uid}", @state + "/" + directory], nil, nil)
    end
    @port = Harness.reserve_port()
    self.write_environment(@state)
    template = File.read(@root + "/applications/registry/deploy/registry.service")
    [
      ["User=diamond-registry", "User=" + @name],
      ["Group=diamond-registry", "Group=" + @name],
      ["StateDirectory=diamond-registry", "StateDirectory=" + @name],
      ["ReadWritePaths=/var/lib/diamond-registry", "ReadWritePaths=" + @state],
      ["WorkingDirectory=/opt/diamond/applications/registry", "WorkingDirectory=" + @app],
      ["EnvironmentFile=/etc/diamond-registry.env", "EnvironmentFile=" + @environment],
      ["ExecStart=/opt/diamond/build/diamond app.di", "ExecStart=" + @work + "/diamond app.di"],
    ].each() do |pair|
      check(template.include?(pair[0]), "the service template no longer contains #{pair[0]}")
      template = template.split(pair[0]).join(pair[1])
    end
    File.publish(@unit, template)
    @unit_created = true
    run(["systemd-analyze", "verify", @unit], nil, nil)
    run(["systemctl", "daemon-reload"], nil, nil)
  end

  def start_and_inspect()
    run(["systemctl", "start", @unit_name], nil, nil)
    pid = self.healthy(nil)
    check(owner_uid("/proc/" + pid.to_s()) == @uid, "the service runs as the wrong account")
    check(self.property_value("ProtectSystem") == "strict", "ProtectSystem is not strict")
    check(self.property_value("NoNewPrivileges") == "yes", "NoNewPrivileges is not set")
    check(owner_uid(@state + "/registry.db") == @uid, "the database is owned by the wrong account")
    check((File.mode(@state + "/registry.db") & 0o077) == 0, "the database is not private")
    puts("systemd startup, service identity, sandbox settings, and private database passed")
    pid
  end

  def publish_and_recover(pid)
    credential_env = {"REGISTRY_ROOT": @state, "REGISTRY_OPERATOR": "systemd-drill", "DIAMOND_NO_CACHE": "1"}
    issued = run(["runuser", "-u", @name, "--", @work + "/diamond", "credentials.di", "issue", "canary-owner",
      "3600", "publish:canary", "systemd drill"], credential_env, @app)
    @token = JSON.parse(issued)["token"]
    @cut = @work + "/canary"
    Harness.mkdir_p(@cut + "/lib")
    File.write(@cut + "/diamond.cut", JSON.stringify({"name": "canary", "version": "1.0.0", "summary": "systemd canary",
      "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}], "dependencies": {}}))
    File.write(@cut + "/README.md", "Systemd staging canary\n")
    File.write(@cut + "/LICENSE", "MIT\n")
    File.write(@cut + "/lib/canary.di", "def canary() = \"restored\"\n")
    archive = @work + "/canary.tar"
    run([@work + "/facet", "pack", @cut, archive], nil, nil)
    @payload = File.read(archive)
    @digest = Digest.sha256(@payload)
    @headers = {"Authorization": "Bearer " + @token, "Content-Type": "application/octet-stream"}
    check(self.request("/v1/cuts/canary/versions", "POST", @payload, @headers)[0] == 201, "the first publish was not a 201")
    run(["systemctl", "restart", @unit_name], nil, nil)
    pid = self.healthy(pid)
    check(self.request("/v1/blobs/sha256/" + @digest, "GET", nil, {}) == [200, @payload], "the blob is gone after a restart")
    run(["systemctl", "kill", "--kill-whom=main", "--signal=SIGKILL", @unit_name], nil, nil)
    pid = self.healthy(pid)
    check(self.property_value("NRestarts").to_i() >= 1, "the crash was not restarted")
    check(self.request("/v1/blobs/sha256/" + @digest, "GET", nil, {}) == [200, @payload], "the blob is gone after a crash")
    puts("explicit restart and automatic crash recovery preserved published data")
  end

  def restore_and_finish()
    snapshot = @work + "/snapshot"
    run([@work + "/diamond", @app + "/backup.di", "backup", @state, snapshot], nil, nil)
    run(["systemctl", "stop", @unit_name], nil, nil)
    check(self.property_value("ActiveState") == "inactive", "the service did not stop")
    recovered = @state + "/recovered"
    run([@work + "/diamond", @app + "/backup.di", "restore", snapshot, recovered], nil, nil)
    run(["chown", "-R", "#{@uid}:#{@uid}", recovered], nil, nil)
    self.write_environment(recovered)
    run(["systemctl", "start", @unit_name], nil, nil)
    self.healthy(nil)
    check(self.request("/v1/blobs/sha256/" + @digest, "GET", nil, {}) == [200, @payload], "the restored blob is gone")
    check(self.request("/v1/cuts/canary/versions", "POST", @payload, @headers)[0] == 200, "the restored publish was not idempotent")
    check(self.request("/v1/cuts/canary/versions", "POST", @payload, {})[0] == 401, "the restored service accepted no credential")
    manifest = JSON.parse(File.read(@cut + "/diamond.cut"))
    manifest["version"] = "1.0.1"
    File.write(@cut + "/diamond.cut", JSON.stringify(manifest))
    run([@work + "/facet", "pack", @cut, @work + "/new-release.tar"], nil, nil)
    new_payload = File.read(@work + "/new-release.tar")
    check(self.request("/v1/cuts/canary/versions", "POST", new_payload, @headers)[0] == 201, "a new release was not a 201")
    check(self.request("/v1/blobs/sha256/" + Digest.sha256(new_payload), "GET", nil, {}) == [200, new_payload], "the new blob is missing")
    journal = run(["journalctl", "-u", @unit_name, "--no-pager", "-o", "cat"], nil, nil)
    check(journal.include?("request.completed") && !journal.include?(@token), "the journal is missing request records or leaks the token")
    run(["systemctl", "stop", @unit_name], nil, nil)
    check(self.property_value("ActiveState") == "inactive", "the restored service did not stop")
    puts("restored systemd service passed blob, credential, write, journal, and clean-stop checks")
  end

  def all()
    self.prepare()
    self.install()
    pid = self.start_and_inspect()
    self.publish_and_recover(pid)
    self.restore_and_finish()
  end
end

if run(["id", "-u"], nil, nil).strip() != "0" || ENV["REGISTRY_QEMU_STAGING"] != "1"
  warn("Run as root with REGISTRY_QEMU_STAGING=1 inside the local staging VM")
  exit(1)
end

drill = Drill.new(root)
begin
  drill.all()
rescue error: StandardError
  puts(drill.journal_tail())
  raise error
ensure
  drill.cleanup()
end
exit(0)
