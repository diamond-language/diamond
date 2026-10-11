def integer(value: Int) -> Int = value
def floating(value: Float) -> Float = value
def boolean(value: Bool) -> Bool = value
def optional(value: Int|Nil) -> Int|Nil = value
def text(value: String) -> String = value
def symbol(value: Symbol) -> Symbol = value
def array(value: Array) -> Array = value
def hash(value: Hash) -> Hash = value
def mixed(value: String|Array|Bool|Nil) -> String|Array|Bool|Nil = value
def constrained(value: Array[Int]|Array) -> Array[Int]|Array = value

def run()
  i = 0
  while i < 20
    unless integer((9223372036854775807 + 1)) == (9223372036854775807 + 1) && integer(i) == i
      raise RuntimeError.new("Int guard lost big integers")
    end
    unless floating(1.5) == 1.5 && boolean(false) == false && optional(nil) == nil
      raise RuntimeError.new("primitive guard mismatch")
    end
    unless text("hello") == "hello" && symbol(:hello) == :hello
      raise RuntimeError.new("object guard mismatch")
    end
    unless array([i])[0] == i && hash({"x": i})["x"] == i
      raise RuntimeError.new("collection guard mismatch")
    end
    unless mixed(false) == false && mixed(nil) == nil && mixed("hi") == "hi" && mixed([i])[0] == i
      raise RuntimeError.new("union guard mismatch")
    end
    values = constrained([i])
    rejected = false
    begin
      values.push("bad")
    rescue error: TypeError
      rejected = true
    end
    unless rejected
      raise RuntimeError.new("generic union failed to attach constraint")
    end
    heterogeneous = constrained(["allowed"])
    heterogeneous.push(false)
    unless heterogeneous[1] == false
      raise RuntimeError.new("plain union fallback attached a constraint")
    end
    i += 1
  end
  bad = [1.5, false, nil, [], {}, "wrong", :wrong]
  j = 0
  failures = 0
  while j < bad.length()
    begin
      integer(bad[j])
    rescue error: TypeError
      failures += 1
    end
    j += 1
  end
  unless failures == bad.length()
    raise RuntimeError.new("mismatched tags bypassed the guard")
  end
  i
end
run()
