# Every plain built-in type guard against every kind of value. The same
# functions run once cold and then repeatedly, so a guard compiled by the
# JIT is checked against exactly what the interpreter's matcher accepts.
def want_int(value: Int) -> Int = value
def want_float(value: Float) -> Float = value
def want_bool(value: Bool) -> Bool = value
def want_nil(value: Nil) -> Nil = value
def want_string(value: String) -> String = value
def want_symbol(value: Symbol) -> Symbol = value
def want_array(value: Array) -> Array = value
def want_hash(value: Hash) -> Hash = value
def want_string_or_nil(value: String | Nil) -> String | Nil = value
def want_int_or_symbol(value: Int | Symbol) -> Int | Symbol = value
def want_float_or_hash(value: Float | Hash) -> Float | Hash = value
def want_bool_or_array(value: Bool | Array) -> Bool | Array = value

# accepts(function_index, value_index) -> whether the guard must accept.
# Values: small int, bignum, float, true, nil, string, symbol, array, hash.
def accepted(row, column)
  table = [
    [true, true, false, false, false, false, false, false, false],
    [false, false, true, false, false, false, false, false, false],
    [false, false, false, true, false, false, false, false, false],
    [false, false, false, false, true, false, false, false, false],
    [false, false, false, false, false, true, false, false, false],
    [false, false, false, false, false, false, true, false, false],
    [false, false, false, false, false, false, false, true, false],
    [false, false, false, false, false, false, false, false, true],
    [false, false, false, false, true, true, false, false, false],
    [true, true, false, false, false, false, true, false, false],
    [false, false, true, false, false, false, false, false, true],
    [false, false, false, true, false, false, false, true, false]
  ]
  table[row][column]
end

def call_guard(row, value)
  if row == 0
    want_int(value)
  elsif row == 1
    want_float(value)
  elsif row == 2
    want_bool(value)
  elsif row == 3
    want_nil(value)
  elsif row == 4
    want_string(value)
  elsif row == 5
    want_symbol(value)
  elsif row == 6
    want_array(value)
  elsif row == 7
    want_hash(value)
  elsif row == 8
    want_string_or_nil(value)
  elsif row == 9
    want_int_or_symbol(value)
  elsif row == 10
    want_float_or_hash(value)
  else
    want_bool_or_array(value)
  end
end

def check_matrix()
  values = [7, 9223372036854775807 + 1, 1.5, true, nil, "s", :s, [1], {"k": 1}]
  round = 0
  while round < 30
    row = 0
    while row < 12
      column = 0
      while column < values.length()
        raised = false
        begin
          call_guard(row, values[column])
        rescue error: TypeError
          raised = true
        end
        if raised == accepted(row, column)
          raise RuntimeError.new("guard #{row} on value #{column}: raised=#{raised}")
        end
        column += 1
      end
      row += 1
    end
    round += 1
  end
  "ok"
end
check_matrix()
