require "lexer"

# Self-hosting Phase 3: a Diamond-language port of src/compiler.c's
# single-pass parser. Uses selfhost/lexer.di for tokenizing and the
# native ProgramBuilder bridge (see docs/roadmap.md's Phase 1 entry) to
# emit and run real bytecode. Built incrementally, sub-phase by
# sub-phase, per the self-hosting roadmap plan's suggested sequencing:
#
#   Sub-phase 1 ("expression evaluator core"): literals, arithmetic/
#   comparison/logical expressions, local variables, if/while/loop/break.
#
#   Sub-phase 2 (this addition): top-level named functions with purely
#   positional parameters (no defaults, no type annotations, no
#   generics), direct calls to already-declared functions (including
#   self-recursion -- a function's own entry is registered before its
#   body is compiled, exactly mirroring compiler.c's own ordering, so
#   forward/mutual recursion between two functions declared in either
#   order has the same "must already exist" constraint the real compiler
#   has), and `puts`/`print`.
#
# Deliberately narrower than the eventual full port throughout:
#   - No closures/nested `def`, classes/methods/`super`, interfaces/
#     generics/narrowing, or exceptions/modules/`require` -- each is a
#     separate later sub-phase. A nested `def` (one not at the top
#     level) is a clear, explicit compile error here, not a silent
#     miscompile.
#   - No keyword arguments, explicit `return`, or compile-time arity
#     defaults -- a function's last expression is always its result.
#   - No compile-time `_INT` opcode quickening (compiler.c's own
#     `known_types` optimization): every arithmetic/comparison opcode
#     emitted here is the generic form (ADD, not ADD_INT), which is
#     always correct and only foregoes a speed optimization the VM would
#     otherwise apply lazily at runtime via the interpreter's own
#     quickening (`DIAMOND_QUICKEN`) instead -- see docs/roadmap.md.
#   - No string interpolation (`"#{...}"`): a string literal's `\n`/`\t`/
#     `\r`/`\"`/`\\`/`\#` escapes are decoded, but a literal `#{` is left
#     as plain text rather than evaluated -- needs the same
#     lexer/compiler-state save-and-restore compiler.c's parse_string
#     uses, deferred until interpolation is specifically in scope.
#   - `next`/`redo` and postfix `if`/`unless` modifiers are not
#     supported; only `break` (with an optional value).
#
# The opcode constants below must exactly match src/vm.h's DiamondOpCode
# enum ordinals -- this file and the VM only ever need to agree the same
# way compiler.c and vm.c already implicitly do (see the ProgramBuilder
# design note in docs/roadmap.md's Phase 1 entry). Verified against a
# real `sizeof`/enum-dump each time this file is touched, not hand-counted.
# Namespace constants (module-scoped, referenced as `Opcode::NAME`) rather
# than plain top-level locals: a top-level `NAME = value` is only an
# ordinary local variable in the top-level script's own register frame,
# invisible from inside a class method's separate function/frame -- module
# constants are the one Diamond mechanism that's actually readable from
# anywhere via qualified `Module::NAME` lookup (DIAMOND_OP_GET_NAMESPACE_
# CONSTANT), regardless of the referencing code's own lexical scope.
module Opcode
  CONSTANT = 0
  STRING = 1
  NIL = 3
  BOOL = 4
  MOVE = 5
  ADD = 6
  SUBTRACT = 8
  MULTIPLY = 9
  DIVIDE = 10
  LESS = 14
  LESS_EQUAL = 15
  GREATER = 16
  GREATER_EQUAL = 17
  NEGATE = 18
  EQUAL = 19
  NOT_EQUAL = 20
  JUMP = 27
  CALL = 29
  JUMP_IF_FALSE = 28
  NOT = 55
  JUMP_IF_TRUE = 56
  RETURN = 57
  PRINT = 70
end

module Precedence
  NONE = 0
  OR = 1
  AND = 2
  EQUALITY = 3
  COMPARISON = 4
  TERM = 5
  FACTOR = 6
  PREFIX = 7
end

class Parser
  def initialize(source, builder)
    @source = source
    @builder = builder
    @lexer = Lexer.new(source)
    @current = @lexer.next_token()
    @previous = @current
    @code_count = 0
    @next_register = 0
    @locals = []
    @loops = []
    @functions = []
    # -1 targets the ProgramBuilder entry function; a real function's own
    # index once compile_definition switches into its body. Every
    # emit_byte/add_constant/add_string/patch_byte call routes through
    # this so the same emission helpers work unmodified regardless of
    # which function is currently being compiled.
    @current_function_index = -1
    @failed = false
    @error_message = nil
  end

  def error_message()
    @error_message
  end

  # Parses and emits the whole program into function -1 (the
  # ProgramBuilder's entry function -- see docs/roadmap.md's Phase 1
  # entry), finishing with a RETURN of the top-level sequence's result.
  # Returns true on success; on failure, error_message() explains why and
  # no .run() should be attempted against a partially-emitted program.
  def compile()
    result = self.compile_sequence()
    if !@failed && @current.kind() != :eof
      self.fail("unexpected block terminator")
    end
    self.emit_instruction1(Opcode::RETURN, result)
    @builder.set_register_count(-1, @next_register)
    !@failed
  end

  private

  def fail(message)
    @error_message = message unless @failed
    @failed = true
  end

  def advance_token()
    @previous = @current
    @current = @lexer.next_token()
  end

  def skip_newlines()
    while @current.kind() == :newline
      self.advance_token()
    end
  end

  def at_block_end?()
    @current.kind() == :eof || @current.kind() == :else || @current.kind() == :elsif || @current.kind() == :end
  end

  def token_text(token)
    @source.slice(token.start(), token.length())
  end

  # --- emission, mirroring compiler.c's emit_byte/emit_opcode/
  # emit_instruction/allocate_register/add_constant/add_string ---

  def emit_byte(byte)
    @builder.emit_byte(@current_function_index, byte)
    @code_count = @code_count + 1
  end

  def emit_instruction1(opcode, a)
    self.emit_byte(opcode)
    self.emit_byte(a)
  end

  def emit_instruction2(opcode, a, b)
    self.emit_byte(opcode)
    self.emit_byte(a)
    self.emit_byte(b)
  end

  def emit_instruction3(opcode, a, b, c)
    self.emit_byte(opcode)
    self.emit_byte(a)
    self.emit_byte(b)
    self.emit_byte(c)
  end

  def allocate_register()
    register = @next_register
    @next_register = @next_register + 1
    register
  end

  def add_constant(value)
    @builder.add_constant(@current_function_index, value)
  end

  def add_string(text)
    @builder.add_string(@current_function_index, text)
  end

  # JUMP: [opcode][hi][lo]. JUMP_IF_FALSE/JUMP_IF_TRUE:
  # [opcode][condition][hi][lo]. `operand` is the offset of the first
  # placeholder byte, recorded before the target is known so patch_jump
  # can overwrite it later -- exactly compiler.c's own emit_jump/patch_jump
  # split, now via ProgramBuilder#patch_byte instead of a direct C array
  # write.
  def emit_jump(opcode, condition)
    conditional = opcode == Opcode::JUMP_IF_FALSE || opcode == Opcode::JUMP_IF_TRUE
    operand = @code_count + (if conditional
      2
    else
      1
    end)
    self.emit_byte(opcode)
    self.emit_byte(condition) if conditional
    self.emit_byte(0)
    self.emit_byte(0)
    operand
  end

  def patch_jump(operand, target)
    @builder.patch_byte(@current_function_index, operand, target / 256)
    @builder.patch_byte(@current_function_index, operand + 1, mod(target, 256))
  end

  def emit_absolute_jump(target)
    self.emit_byte(Opcode::JUMP)
    self.emit_byte(target / 256)
    self.emit_byte(mod(target, 256))
  end

  # --- locals: an Array of [name, register] pairs, scanned in reverse so
  # the most recently declared match wins on shadowing (mirroring
  # compiler.c's find_local's own reverse scan over its fixed-size
  # array) -- no enclosing-scope/capture support yet, unlike the C
  # original, since closures are a separate later sub-phase. ---

  def find_local(name)
    index = @locals.length() - 1
    result = -1
    while index >= 0 && result == -1
      pair = @locals[index]
      result = pair[1] if pair[0] == name
      index = index - 1
    end
    result
  end

  def define_local(name)
    register = self.allocate_register()
    @locals.push([name, register])
    register
  end

  # --- functions: an Array of [name, function_index, arity] entries,
  # scanned the same reverse-shadowing way as locals (compiler.c's own
  # find_function scans its fixed-size program->functions array forward,
  # but since redeclaration is already rejected in compile_definition,
  # forward vs. reverse never actually differs in practice here). ---

  def find_function(name)
    index = @functions.length() - 1
    result = nil
    while index >= 0 && result == nil
      entry = @functions[index]
      result = entry if entry[0] == name
      index = index - 1
    end
    result
  end

  # --- statement sequencing ---

  def compile_sequence()
    self.skip_newlines()
    result = self.allocate_register()
    while !@failed && !self.at_block_end?()
      if @current.kind() == :def
        result = self.compile_definition()
      elsif @current.kind() == :break
        result = self.compile_break()
      elsif self.assignment_ahead?()
        result = self.compile_assignment()
      else
        result = self.parse_expression()
      end
      if @current.kind() == :newline
        self.skip_newlines()
      elsif !self.at_block_end?()
        self.fail("expected newline after expression")
      end
    end
    result
  end

  # Top-level named functions only (see this file's header comment for
  # the full scope cut): purely positional parameters, no defaults, no
  # forward references except a function calling itself (its own entry
  # is registered in @functions before its body is compiled, exactly
  # mirroring compiler.c's own ordering).
  def compile_definition()
    self.advance_token()
    if @current_function_index != -1
      self.fail("nested function definitions are not yet supported")
      return 0
    end
    if @current.kind() != :identifier
      self.fail("expected function name after 'def'")
      return 0
    end
    name = self.token_text(@current)
    if self.find_function(name) != nil
      self.fail("function is already defined")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after function name")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    parameter_names = []
    if @current.kind() != :right_paren
      more = true
      while more && !@failed
        if @current.kind() != :identifier
          self.fail("expected parameter name")
          more = false
        else
          parameter_names.push(self.token_text(@current))
          self.advance_token()
          self.skip_newlines()
          if @current.kind() == :comma
            self.advance_token()
            self.skip_newlines()
            more = @current.kind() != :right_paren
          else
            more = false
          end
        end
      end
    end
    return 0 if @failed
    if @current.kind() != :right_paren
      self.fail("expected ')' after parameters")
      return 0
    end
    self.advance_token()

    arity = parameter_names.length()
    function_index = @builder.declare_function(name, arity, arity)
    @functions.push([name, function_index, arity])

    outer_locals = @locals
    outer_loops = @loops
    outer_next_register = @next_register
    outer_code_count = @code_count
    outer_function_index = @current_function_index

    @locals = []
    @loops = []
    @next_register = 0
    @code_count = 0
    @current_function_index = function_index

    index = 0
    while index < parameter_names.length()
      self.define_local(parameter_names[index])
      index = index + 1
    end

    if self.consume_block_start()
      body_result = self.compile_sequence()
      self.emit_instruction1(Opcode::RETURN, body_result)
      if @current.kind() != :end
        self.fail("expected 'end' after function body")
      else
        self.advance_token()
      end
    end
    @builder.set_register_count(function_index, @next_register)

    @locals = outer_locals
    @loops = outer_loops
    @next_register = outer_next_register
    @code_count = outer_code_count
    @current_function_index = outer_function_index

    # Top-level def evaluates to nil, a sole-writer fresh register in the
    # (now-restored) outer function -- matching compile_definition.c's
    # own at_top_level branch exactly (no opcode needed).
    self.allocate_register()
  end

  def compile_call(name)
    function_entry = self.find_function(name)
    if function_entry == nil
      self.fail("undefined function")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    arguments = []
    if @current.kind() != :right_paren
      more = true
      while more
        arguments.push(self.parse_expression())
        self.skip_newlines()
        if @current.kind() == :comma
          self.advance_token()
          self.skip_newlines()
          more = @current.kind() != :right_paren
        else
          more = false
        end
      end
    end
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    if arguments.length() != function_entry[2]
      self.fail("wrong number of arguments")
      return 0
    end
    argument_base = self.allocate_register()
    i = 1
    while i < arguments.length()
      self.allocate_register()
      i = i + 1
    end
    i = 0
    while i < arguments.length()
      self.emit_instruction2(Opcode::MOVE, argument_base + i, arguments[i])
      i = i + 1
    end
    destination = self.allocate_register()
    self.emit_byte(Opcode::CALL)
    self.emit_byte(destination)
    self.emit_byte(function_entry[1])
    self.emit_byte(argument_base)
    self.emit_byte(arguments.length())
    destination
  end

  def parse_print_call(newline)
    self.advance_token()
    source = self.parse_expression()
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_byte(Opcode::PRINT)
    self.emit_byte(destination)
    self.emit_byte(source)
    self.emit_byte(if newline
      1
    else
      0
    end)
    destination
  end

  def assignment_ahead?()
    return false if @current.kind() != :identifier
    lookahead = @lexer.clone()
    lookahead.next_token().kind() == :equal
  end

  def compile_assignment()
    name = self.token_text(@current)
    self.advance_token()
    self.advance_token()
    value = self.parse_expression()
    existing = self.find_local(name)
    destination = if existing == -1
      self.define_local(name)
    else
      existing
    end
    self.emit_instruction2(Opcode::MOVE, destination, value)
    destination
  end

  def compile_break()
    if @loops.length() == 0
      self.fail("'break' used outside a loop")
      return self.allocate_register()
    end
    self.advance_token()
    has_value = @current.kind() != :newline && @current.kind() != :end && @current.kind() != :else && @current.kind() != :eof
    frame = @loops[@loops.length() - 1]
    if has_value
      value = self.parse_expression()
      self.emit_instruction2(Opcode::MOVE, frame[0], value)
    end
    frame[1].push(self.emit_jump(Opcode::JUMP, 0))
    self.allocate_register()
  end

  # --- control flow ---

  def consume_conditional_start()
    return false unless self.consume_block_start_or(:then)
    true
  end

  def consume_loop_start()
    return false unless self.consume_block_start_or(:do)
    true
  end

  def consume_block_start_or(delimiter)
    if @current.kind() == delimiter
      self.advance_token()
      self.skip_newlines()
      return true
    end
    self.consume_block_start()
  end

  def consume_block_start()
    if @current.kind() != :newline
      self.fail("expected newline before block body")
      return false
    end
    self.skip_newlines()
    true
  end

  def parse_if(inverted)
    condition = self.parse_expression()
    return 0 unless self.consume_conditional_start()
    branch_condition = condition
    if inverted
      branch_condition = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, branch_condition, condition)
    end
    false_jump = self.emit_jump(Opcode::JUMP_IF_FALSE, branch_condition)
    destination = self.allocate_register()
    then_result = self.compile_sequence()
    self.emit_instruction2(Opcode::MOVE, destination, then_result)
    end_jump = self.emit_jump(Opcode::JUMP, 0)
    self.patch_jump(false_jump, @code_count)

    end_consumed = false
    if @current.kind() == :else
      self.advance_token()
      self.skip_newlines() if @current.kind() == :newline
      else_result = self.compile_sequence()
      self.emit_instruction2(Opcode::MOVE, destination, else_result)
    elsif @current.kind() == :elsif
      self.advance_token()
      else_result = self.parse_if(false)
      self.emit_instruction2(Opcode::MOVE, destination, else_result)
      end_consumed = true
    else
      self.emit_instruction1(Opcode::NIL, destination)
    end

    if !end_consumed && @current.kind() != :end
      self.fail("expected 'end' after if expression")
      return destination
    end
    self.advance_token() unless end_consumed
    self.patch_jump(end_jump, @code_count)
    destination
  end

  def patch_breaks(frame)
    breaks = frame[1]
    index = 0
    while index < breaks.length()
      self.patch_jump(breaks[index], @code_count)
      index = index + 1
    end
  end

  def parse_while(inverted)
    destination = self.allocate_register()
    self.emit_instruction1(Opcode::NIL, destination)
    loop_start = @code_count
    condition = self.parse_expression()
    return 0 unless self.consume_loop_start()
    branch_condition = condition
    if inverted
      branch_condition = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, branch_condition, condition)
    end
    exit_jump = self.emit_jump(Opcode::JUMP_IF_FALSE, branch_condition)
    frame = [destination, []]
    @loops.push(frame)
    self.compile_sequence()
    @loops.pop()
    self.emit_absolute_jump(loop_start)
    self.patch_jump(exit_jump, @code_count)
    self.patch_breaks(frame)
    if @current.kind() != :end
      self.fail("expected 'end' after while expression")
      return 0
    end
    self.advance_token()
    destination
  end

  def parse_loop()
    destination = self.allocate_register()
    self.emit_instruction1(Opcode::NIL, destination)
    return destination unless self.consume_loop_start()
    body_start = @code_count
    frame = [destination, []]
    @loops.push(frame)
    self.compile_sequence()
    @loops.pop()
    self.emit_absolute_jump(body_start)
    self.patch_breaks(frame)
    if @current.kind() != :end
      self.fail("expected 'end' after loop")
      return destination
    end
    self.advance_token()
    destination
  end

  # --- expressions (Pratt/precedence-climbing, mirroring
  # parse_precedence/parse_prefix/token_precedence/binary_opcode) ---

  def token_precedence(kind)
    return Precedence::OR if kind == :or_or || kind == :or
    return Precedence::AND if kind == :and_and || kind == :and
    return Precedence::EQUALITY if kind == :equal_equal || kind == :bang_equal
    return Precedence::COMPARISON if kind == :less || kind == :less_equal || kind == :greater || kind == :greater_equal
    return Precedence::TERM if kind == :plus || kind == :minus
    return Precedence::FACTOR if kind == :star || kind == :slash
    Precedence::NONE
  end

  def binary_opcode(kind)
    return Opcode::ADD if kind == :plus
    return Opcode::SUBTRACT if kind == :minus
    return Opcode::MULTIPLY if kind == :star
    return Opcode::DIVIDE if kind == :slash
    return Opcode::EQUAL if kind == :equal_equal
    return Opcode::NOT_EQUAL if kind == :bang_equal
    return Opcode::LESS if kind == :less
    return Opcode::LESS_EQUAL if kind == :less_equal
    return Opcode::GREATER if kind == :greater
    Opcode::GREATER_EQUAL
  end

  def parse_expression()
    self.parse_precedence(Precedence::OR)
  end

  def parse_precedence(precedence)
    left = self.parse_prefix()
    while !@failed && self.token_precedence(@current.kind()) >= precedence
      operator = @current.kind()
      operator_precedence = self.token_precedence(operator)
      self.advance_token()
      if operator == :and_and || operator == :and || operator == :or_or || operator == :or
        is_and = operator == :and_and || operator == :and
        destination = self.allocate_register()
        self.emit_instruction2(Opcode::MOVE, destination, left)
        end_jump = self.emit_jump(if is_and
          Opcode::JUMP_IF_FALSE
        else
          Opcode::JUMP_IF_TRUE
        end, left)
        right = self.parse_precedence(operator_precedence + 1)
        self.emit_instruction2(Opcode::MOVE, destination, right)
        self.patch_jump(end_jump, @code_count)
        left = destination
      else
        right = self.parse_precedence(operator_precedence + 1)
        destination = self.allocate_register()
        self.emit_instruction3(self.binary_opcode(operator), destination, left, right)
        left = destination
      end
    end
    left
  end

  def parse_prefix()
    self.advance_token()
    kind = @previous.kind()
    return self.parse_integer() if kind == :integer
    return self.parse_float() if kind == :float
    return self.parse_string() if kind == :string
    return self.parse_literal() if kind == :true || kind == :false || kind == :nil
    return self.parse_name() if kind == :identifier
    return self.parse_grouping() if kind == :left_paren
    if kind == :minus
      operand = self.parse_precedence(Precedence::PREFIX)
      destination = self.allocate_register()
      self.emit_instruction2(Opcode::NEGATE, destination, operand)
      return destination
    end
    if kind == :bang || kind == :not
      operand = self.parse_precedence(Precedence::PREFIX)
      destination = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, destination, operand)
      return destination
    end
    return self.parse_if(false) if kind == :if
    return self.parse_if(true) if kind == :unless
    return self.parse_while(false) if kind == :while
    return self.parse_while(true) if kind == :until
    return self.parse_loop() if kind == :loop
    self.fail("expected expression")
    0
  end

  def parse_grouping()
    result = self.parse_expression()
    if @current.kind() != :right_paren
      self.fail("expected ')' after expression")
      return result
    end
    self.advance_token()
    result
  end

  # Dispatch precedence mirrors parse_name/parse_call in compiler.c: a
  # local variable always shadows a same-named builtin or function (so
  # `puts = 1; puts(2)` is a runtime TypeError on calling an Int, not a
  # print), builtins are checked before user functions (matching
  # `find_local(name)<0&&find_function(name)<0&&name_equals(...,"puts")`
  # exactly), and a bare identifier with no following `(` is always a
  # local read.
  def parse_name()
    name = self.token_text(@previous)
    local = self.find_local(name)
    if @current.kind() == :left_paren && local == -1
      return self.parse_print_call(true) if name == "puts"
      return self.parse_print_call(false) if name == "print"
      return self.compile_call(name)
    end
    if local == -1
      self.fail("undefined local variable")
      return 0
    end
    local
  end

  def parse_literal()
    destination = self.allocate_register()
    if @previous.kind() == :nil
      # Sole-writer register; run_chunk's zero-init already covers nil,
      # matching compiler.c's parse_literal (no NIL opcode emitted here).
    else
      value = if @previous.kind() == :true
        1
      else
        0
      end
      self.emit_instruction2(Opcode::BOOL, destination, value)
    end
    destination
  end

  def parse_integer()
    text = self.token_text(@previous)
    digits = ""
    index = 0
    while index < text.length()
      character = text[index]
      digits = digits + character unless character == "_"
      index = index + 1
    end
    destination = self.allocate_register()
    constant = self.add_constant(digits.to_i())
    self.emit_instruction2(Opcode::CONSTANT, destination, constant)
    destination
  end

  def parse_float()
    text = self.token_text(@previous)
    digits = ""
    index = 0
    while index < text.length()
      character = text[index]
      digits = digits + character unless character == "_"
      index = index + 1
    end
    destination = self.allocate_register()
    constant = self.add_constant(digits.to_f())
    self.emit_instruction2(Opcode::CONSTANT, destination, constant)
    destination
  end

  def decode_string(token)
    raw = @source.slice(token.start() + 1, token.length() - 2)
    result = ""
    index = 0
    while index < raw.length()
      character = raw[index]
      if character == "\\" && index + 1 < raw.length()
        index = index + 1
        escape = raw[index]
        if escape == "n"
          result = result + "\n"
        elsif escape == "r"
          result = result + "\r"
        elsif escape == "t"
          result = result + "\t"
        elsif escape == "\"" || escape == "\\" || escape == "#"
          result = result + escape
        else
          self.fail("unsupported string escape")
        end
      else
        result = result + character
      end
      index = index + 1
    end
    result
  end

  def parse_string()
    text = self.decode_string(@previous)
    destination = self.allocate_register()
    constant = self.add_string(text)
    self.emit_instruction2(Opcode::STRING, destination, constant)
    destination
  end
end

# Convenience entry point mirroring diamond_compile's own two-step shape
# (build, then execute via diamond_program_chunk + diamond_vm_run) --
# here, ProgramBuilder#run does both at once. Returns the parser on
# failure (so the caller can read .error_message()) or runs the program
# and returns its result on success.
def parse_and_run(source)
  builder = ProgramBuilder.new()
  parser = Parser.new(source, builder)
  if parser.compile()
    builder.run()
  else
    raise RuntimeError.new(parser.error_message())
  end
end
