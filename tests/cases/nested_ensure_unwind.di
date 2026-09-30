def normal()
  begin
    7
  ensure
    begin
      1
    rescue error
      2
    end
  end
end

def preserved_exception()
  begin
    begin
      raise "outer"
    ensure
      begin
        raise "inner"
      rescue error
        [error, "temporary"]
      ensure
        ["nested cleanup"]
      end
    end
  rescue error
    error
  end
end

def preserved_return()
  begin
    return ["kept", 42]
  ensure
    begin
      raise nil
    rescue error
      100.times() do |i| [i, "allocate #{i}"] end
    ensure
      ["deep cleanup"]
    end
  end
end

def override_return()
  begin
    return 1
  ensure
    begin
      return 9
    ensure
      begin
        2
      rescue error
        3
      end
    end
  end
end

def override_exception()
  begin
    begin
      raise "old"
    ensure
      begin
        raise "new"
      ensure
        ["cleanup"]
      end
    end
  rescue error
    error
  end
end

def retried_cleanup()
  tries = 0
  begin
    11
  ensure
    begin
      tries += 1
      raise "again" if tries < 3
    rescue error
      retry
    end
  end
end

# A handled block break inside cleanup must not destroy an in-flight
# non-local return's target/value while it crosses another frame.
def invoke_with_cleanup(callback)
  begin
    callback()
  ensure
    begin
      [1].each() do |item| break "inner break" end
    ensure
      ["allocated during pending non-local exit"]
    end
  end
end

def nonlocal_return()
  invoke_with_cleanup() do
    return ["returned"]
  end
  "unreachable"
end

broken = invoke_with_cleanup() do
  break ["broken"]
end
[normal(), preserved_exception(), preserved_return(), override_return(),
 override_exception(), retried_cleanup(), nonlocal_return(), broken]
