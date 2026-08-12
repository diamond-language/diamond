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
#   Sub-phases 2-5 extend that core with named functions and closures,
#   classes and interfaces, typed and generic declarations, collection
#   literals and indexing, namespaces/modules and `require`, and exception
#   handling. The differential corpus in tests/parser_cases records the
#   exact supported surface and grows with each slice.
#
# Deliberately narrower than the eventual full port throughout:
#   - Unsupported constructs fail explicitly; the port must never silently
#     accept a construct it cannot lower with native-compiler semantics.
#   - No compile-time `_INT` opcode quickening (compiler.c's own
#     `known_types` optimization): every arithmetic/comparison opcode
#     emitted here is the generic form (ADD, not ADD_INT), which is
#     always correct and only foregoes a speed optimization the VM would
#     otherwise apply lazily at runtime via the interpreter's own
#     quickening (`DIAMOND_QUICKEN`) instead -- see docs/roadmap.md.
#   - Remaining grammar and semantic gaps are tracked by the Phase 3 roadmap
#     and are added as independently differential-tested slices.
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
  CALL_TYPED = 30
  JUMP_IF_FALSE = 28
  CLOSURE = 31
  CALL_CLOSURE = 32
  GET_CAPTURE_CELL = 34
  BOX_LOCAL = 36
  GET_CELL = 37
  SET_CELL = 38
  NEW = 39
  INVOKE = 40
  SUPER = 43
  GET_IVAR = 44
  SET_IVAR = 45
  CHECK_TYPE = 50
  ARRAY = 51
  INDEX_GET = 52
  INDEX_SET = 53
  HASH = 54
  NOT = 55
  JUMP_IF_TRUE = 56
  RETURN = 57
  RAISE = 58
  PUSH_RESCUE = 59
  POP_RESCUE = 60
  PUSH_ENSURE = 61
  RUN_ENSURE = 62
  END_ENSURE = 63
  IS_TYPE = 64
  TO_STRING = 66
  FIBER_NEW = 69
  PRINT = 70
  GETS = 71
  FILE_OPEN = 72
  TCP_CONNECT = 73
  TCP_LISTEN = 74
  REGEXP_NEW = 75
  CHR = 76
  TO_FLOAT = 77
  TO_INT = 78
  TO_SYMBOL = 79
  MATH_UNARY = 80
  MATH_BINARY = 81
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

# Phase 3 sub-phase 4 (gradual typing): scalar and union type annotations
# (`Int`/`Float`/`String`/`Bool`/`Nil`/a declared class name, separated by
# `|`) plus recursively typed `Array[T]` and `Hash[K, V]` contracts on
# parameters and return types, plus `Callable[n]`, `Callable[n, Return]`,
# and `Callable[[Parameters], Return]`, plus top-level structural interface
# declarations. Deliberately no interface inheritance, generics, or
# narrowing yet --
# each is a natural, separate extension of the same
# ProgramBuilder#declare_type_set bridge method this round adds, not
# something this round itself needs. Values must exactly match
# src/vm.h's DiamondTypeId enum ordinals, the same way Opcode above
# does for DiamondOpCode.
module Type
  INT = 0
  FLOAT = 1
  STRING = 2
  BOOL = 3
  NIL = 4
  ARRAY = 5
  HASH = 6
  CALLABLE = 7
  SYMBOL = 9
  CLASS_BASE = 10
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
    # Each entry is [name, register, captured] -- captured marks a local
    # whose register holds a Cell (either a nested closure captured it,
    # or this local itself is a captured-from-outside binding inside a
    # nested function's own body), needing GET_CELL/SET_CELL rather than
    # a direct register read/MOVE. See compile_definition/BOX_LOCAL.
    @locals = []
    @loops = []
    @functions = []
    # -1 targets the ProgramBuilder entry function; a real function's own
    # index once compile_definition switches into its body. Every
    # emit_byte/add_constant/add_string/patch_byte call routes through
    # this so the same emission helpers work unmodified regardless of
    # which function is currently being compiled.
    @current_function_index = -1
    # The enclosing function's own @locals snapshot, taken the moment a
    # nested `def` is entered -- every entry becomes a capture candidate
    # (see compile_definition). Empty outside a nested function's body.
    @enclosing_locals = []
    # 0 while compiling the top-level script -- a `def` encountered here
    # is a plain top-level function (compile_definition's own
    # at_top_level check reads this *before* incrementing). Entering any
    # function body increments it; a `def` encountered at depth 1
    # (inside a top-level function's own body) becomes a nested closure
    # and is allowed, but a `def` encountered at depth 2 (inside that
    # closure's own body) is rejected -- a deliberate scope cut
    # supporting exactly one level of function nesting, not the
    # arbitrary depth compiler.c's own general enclosing_locals-walk
    # supports.
    @function_nesting_depth = 0
    # Array of [name, class_index] entries, mirroring @functions --
    # `ClassName.new(...)` resolves against this via find_class.
    @classes = []
    @interfaces = []
    @modules = []
    @current_module_index = nil
    @current_module_name = nil
    @current_module_entry = nil
    @current_type_variables = []
    @type_facts = []
    @declared_types = []
    @pending_nil_narrowing = nil
    @pending_type_narrowing = nil
    @current_exception = nil
    @current_retry_target = nil
    # The class_index currently being compiled (compile_class), or nil
    # outside any class body -- gates `self`/`@ivar` (both require being
    # inside a method) and rejects a class or `def` nested inside a
    # class body beyond one plain method (see compile_class/compile_method).
    @current_class_index = nil
    # Names already declared as methods on the class currently being
    # compiled, checked before declare_method so a duplicate method
    # produces this Parser's own fail()/error_message() instead of
    # surfacing as an uncaught exception from declare_method's own
    # (separate, VM-level) duplicate check.
    @current_class_method_names = []
    # The class_index of the class currently being compiled's
    # superclass, or nil if it has none -- gates `super`'s own "used in
    # a class without a superclass" check. Doesn't need the superclass's
    # full identity beyond "does one exist": SUPER's own opcode operand
    # is the *current* class's index, not the superclass's -- the VM
    # itself walks `owner->superclass` at runtime (see docs/roadmap.md).
    @current_class_superclass_index = nil
    # The name of the method currently being compiled (compile_method),
    # needed by `super(...)`: it always calls the superclass's version
    # of *this same* method, never an explicitly named one.
    @current_method_name = nil
    @current_method_uses_state = false
    # The current function/method/closure's declared `-> Type` return
    # type name (a String), or nil if it has none -- needed by both the
    # implicit final-expression return path (compile_function_body/
    # compile_method_body) and every explicit `return` statement
    # (compile_return) anywhere in its body, mirroring compiler.c's own
    # current_return_type field exactly (checked on *every* return path,
    # not just the implicit one).
    @current_return_type = nil
    @failed = false
    @error_message = nil
    self.fail("unexpected character") if @current.kind() == :error
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
    if !@failed
      @error_message = @builder.source_location(@current.start(), @current.line(), @current.column()) + ": " + message
    end
    @failed = true
  end

  def advance_token()
    @previous = @current
    @current = @lexer.next_token()
    self.fail("unexpected character") if @current.kind() == :error
  end

  def skip_newlines()
    while @current.kind() == :newline
      self.advance_token()
    end
  end

  def at_block_end?()
    @current.kind() == :eof || @current.kind() == :else || @current.kind() == :elsif || @current.kind() == :rescue || @current.kind() == :ensure || @current.kind() == :end
  end

  def token_text(token)
    @source.slice(token.start(), token.length())
  end

  # --- emission, mirroring compiler.c's emit_byte/emit_opcode/
  # emit_instruction/allocate_register/add_constant/add_string ---

  def emit_byte(byte)
    @builder.set_source_location(@previous.line(), @previous.column())
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

  def emit_rescue_handler(exception)
    self.emit_byte(Opcode::PUSH_RESCUE)
    self.emit_byte(exception)
    self.emit_byte(128)
    index = 0
    while index < 8
      self.emit_byte(0)
      index = index + 1
    end
    operand = @code_count
    self.emit_byte(0)
    self.emit_byte(0)
    operand
  end

  def parse_rescue_types(exception)
    types = []
    annotation = []
    if @current.kind() == :colon
      self.advance_token()
      while !@failed
        if @current.kind() != :identifier
          self.fail("expected rescue type")
        elsif types.length() == 8
          self.fail("too many rescue types")
        else
          name = self.token_text(@current)
          type_id = self.resolve_type_name(name)
          if type_id >= 96 && type_id < 128
            self.fail("generic type variables cannot filter rescue")
          end
          duplicate = false
          index = 0
          while index < types.length()
            duplicate = true if types[index] == type_id
            index = index + 1
          end
          self.fail("duplicate rescue type") if duplicate
          types.push(type_id)
          annotation.push([name, nil, nil, -1, nil, nil])
          self.advance_token()
          if @current.kind() == :pipe
            self.advance_token()
          else
            break
          end
        end
      end
    end
    self.set_type_fact(exception, types[0]) if types.length() == 1
    @declared_types.push([exception, annotation]) if annotation.length() > 0
    types
  end

  # --- locals: an Array of [name, register, captured] entries, scanned
  # in reverse so the most recently declared match wins on shadowing
  # (mirroring compiler.c's find_local's own reverse scan over its
  # fixed-size array). find_local returns the entry itself (or nil), not
  # just a register, since callers need `captured` to decide between a
  # direct register read/MOVE and a GET_CELL/SET_CELL unwrap -- see
  # read_local/compile_assignment. ---

  def find_local(name)
    index = @locals.length() - 1
    result = nil
    while index >= 0 && result == nil
      entry = @locals[index]
      result = entry if entry[0] == name
      index = index - 1
    end
    result
  end

  def define_local(name)
    register = self.allocate_register()
    @locals.push([name, register, false])
    register
  end

  # A captured local's register holds a Cell (see compile_definition's
  # GET_CAPTURE_CELL loop), so reading its value needs an extra GET_CELL
  # unwrap; an ordinary local's register already holds the value directly.
  def read_local(entry)
    return entry[1] unless entry[2]
    destination = self.allocate_register()
    self.emit_instruction2(Opcode::GET_CELL, destination, entry[1])
    destination
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

  # --- classes: an Array of [name, class_index, superclass_index] entries. ---

  def find_class(name)
    index = @classes.length() - 1
    result = nil
    while index >= 0 && result == nil
      entry = @classes[index]
      result = entry if entry[0] == name
      index = index - 1
    end
    result
  end

  def find_interface(name)
    index = @interfaces.length() - 1
    result = nil
    while index >= 0 && result == nil
      entry = @interfaces[index]
      result = entry if entry[0] == name
      index = index - 1
    end
    result
  end

  # --- statement sequencing ---

  def postfix_modifier_ahead()
    return nil if @current.kind() == :def || @current.kind() == :class || @current.kind() == :interface || @current.kind() == :module
    lookahead = @lexer.clone()
    depth = if @current.kind() == :left_paren || @current.kind() == :left_bracket || @current.kind() == :left_brace
      1
    else
      0
    end
    expression_expected = false
    while true
      kind = lookahead.next_token().kind()
      return nil if kind == :eof || kind == :error || kind == :newline
      if depth == 0 && (kind == :if || kind == :unless)
        return nil if expression_expected
        return kind
      end
      if kind == :left_paren || kind == :left_bracket || kind == :left_brace
        depth = depth + 1
      elsif kind == :right_paren || kind == :right_bracket || kind == :right_brace
        depth = depth - 1 if depth > 0
      end
      expression_expected = depth == 0 && kind == :equal
    end
    nil
  end

  def compile_sequence()
    self.skip_newlines()
    result = self.allocate_register()
    while !@failed && !self.at_block_end?()
      postfix = self.postfix_modifier_ahead()
      has_postfix = postfix == :if || postfix == :unless
      postfix_result = if has_postfix
        self.allocate_register()
      else
        result
      end
      condition_jump = if has_postfix
        self.emit_jump(Opcode::JUMP, 0)
      else
        0
      end
      body_start = @code_count
      declaration_statement = @current.kind() == :def || @current.kind() == :class || @current.kind() == :interface || @current.kind() == :module
      if @current.kind() == :def
        result = self.compile_definition()
      elsif @current.kind() == :class
        result = self.compile_class()
      elsif @current.kind() == :interface
        result = self.compile_interface()
      elsif @current.kind() == :module
        result = self.compile_module()
      elsif @current.kind() == :break || @current.kind() == :next || @current.kind() == :redo
        result = self.compile_break()
      elsif @current.kind() == :return
        result = self.compile_return()
      elsif @current.kind() == :raise
        result = self.compile_raise()
      elsif @current.kind() == :retry
        result = self.compile_retry()
      elsif self.index_assignment_ahead?()
        result = self.compile_index_assignment()
      elsif self.assignment_ahead?()
        result = self.compile_assignment()
      else
        result = self.parse_expression()
      end
      if has_postfix
        if @current.kind() != postfix
          self.fail("expected postfix condition")
          return result
        end
        self.advance_token()
        self.emit_instruction2(Opcode::MOVE, postfix_result, result)
        body_exit = self.emit_jump(Opcode::JUMP, 0)
        condition_start = @code_count
        condition = self.parse_expression()
        body_jump = self.emit_jump(if postfix == :if
          Opcode::JUMP_IF_TRUE
        else
          Opcode::JUMP_IF_FALSE
        end, condition)
        self.emit_instruction1(Opcode::NIL, postfix_result)
        self.patch_jump(condition_jump, condition_start)
        self.patch_jump(body_exit, @code_count)
        self.patch_jump(body_jump, body_start)
        result = postfix_result
      end
      if !has_postfix && declaration_statement && (@current.kind() == :if || @current.kind() == :unless)
        self.fail("postfix modifiers cannot follow declarations")
      elsif @current.kind() == :newline
        self.skip_newlines()
      elsif !self.at_block_end?()
        self.fail("expected newline after expression")
      end
    end
    result
  end

  # Top-level named functions (purely positional parameters, no
  # defaults) and exactly one level of nested closures -- see this
  # file's header comment and @function_nesting_depth's own comment for
  # the full scope. A top-level function is found later by name via
  # @functions/find_function and called with CALL; a nested `def`
  # instead becomes a local variable (named after the function) holding
  # a Closure value, called with CALL_CLOSURE like any other
  # closure-valued local -- compiler.c's own distinction, mirrored
  # exactly (`at_top_level`, computed from @function_nesting_depth
  # *before* it's incremented for this def's own body).
  # Split into several small methods (parse_parameter_names/
  # compile_function_body/emit_closure below), not just for readability:
  # every local variable anywhere in one method body counts toward that
  # method's own 256-register budget (register allocation is monotonic,
  # never recycled -- see Compiler.next_register in compiler.c), and the
  # first, single-method version of this exhausted it outright ("program
  # needs too many registers"), the exact same failure mode
  # selfhost/lexer.di's own next_token/scan_punctuation split hit and
  # documented in Phase 2. Worth remembering for the rest of this port:
  # any method this size needs to be split from the start, not after
  # hitting the limit.
  def compile_definition()
    self.advance_token()
    if @function_nesting_depth >= 2
      self.fail("only one level of function nesting is supported")
      return 0
    end
    at_top_level = @function_nesting_depth == 0
    if @current.kind() != :identifier
      self.fail("expected function name after 'def'")
      return 0
    end
    name = self.token_text(@current)
    if at_top_level && self.find_function(name) != nil
      self.fail("function is already defined")
      return 0
    end
    self.advance_token()
    outer_type_variables = @current_type_variables
    @current_type_variables = self.parse_type_variables()
    return 0 if @failed
    if @current.kind() != :left_paren
      self.fail("expected '(' after function name")
      return 0
    end
    self.advance_token()
    parsed_parameters = self.parse_parameter_names()
    return 0 if @failed
    if @current.kind() != :right_paren
      self.fail("expected ')' after parameters")
      return 0
    end
    self.advance_token()
    parameter_names = parsed_parameters[0]
    parameter_types = parsed_parameters[1]
    return_type = nil
    if @current.kind() == :arrow
      self.advance_token()
      return_type = self.parse_type_annotation()
    end
    return 0 if @failed

    arity = parameter_names.length()
    function_index = @builder.declare_function(name, arity, arity)
    @builder.set_type_variables(function_index, @current_type_variables)
    if at_top_level
      @functions.push([name, function_index, arity, parameter_names,
                       @current_type_variables.length(), return_type])
    end
    # Every entry currently in scope becomes a capture candidate,
    # unconditionally -- mirroring compiler.c's own eager design (not
    # just names the nested body actually goes on to reference). Empty
    # for a top-level def (nothing to capture from the top-level script
    # itself, and top-level functions don't capture anything anyway).
    enclosing_locals = if at_top_level
      []
    else
      @locals
    end

    self.compile_function_body(function_index, parameter_names, parameter_types,
                               return_type, enclosing_locals)
    @current_type_variables = outer_type_variables

    if at_top_level
      # Evaluates to nil, a sole-writer fresh register in the
      # (now-restored) outer function -- matching compile_definition.c's
      # own at_top_level branch exactly (no opcode needed).
      return self.allocate_register()
    end
    self.emit_closure(function_index, enclosing_locals, name)
  end

  def parse_type_variables()
    variables = []
    return variables unless @current.kind() == :left_bracket
    self.advance_token()
    self.skip_newlines()
    while !@failed && @current.kind() != :right_bracket
      if @current.kind() != :identifier || variables.length() == 8
        self.fail("expected generic type parameter")
      else
        name = self.token_text(@current)
        index = 0
        while index < variables.length()
          self.fail("duplicate generic type parameter") if variables[index] == name
          index = index + 1
        end
        variables.push(name) unless @failed
        self.advance_token() unless @failed
        self.skip_newlines()
        if @current.kind() == :comma
          self.advance_token()
          self.skip_newlines()
        else
          break
        end
      end
    end
    if !@failed && @current.kind() != :right_bracket
      self.fail("expected ']' after generic type parameters")
    else
      self.advance_token() unless @failed
    end
    variables
  end

  # Returns [names, types]: parallel arrays, `types[i]` is a recursive
  # annotation tree, or nil if it
  # had no `: Type` annotation. Resolving a type name to a type id and
  # declaring its type set happens later, inside the callee's own
  # switched-in context (see compile_function_body/emit_parameter_type_checks)
  # -- declare_type_set needs the *callee's* function_index, which isn't
  # known yet while still parsing the parameter list in the caller's
  # (outer) context.
  def parse_parameter_names()
    self.skip_newlines()
    names = []
    types = []
    if @current.kind() != :right_paren
      more = true
      while more && !@failed
        if @current.kind() != :identifier
          self.fail("expected parameter name")
          more = false
        else
          names.push(self.token_text(@current))
          self.advance_token()
          type_name = nil
          if @current.kind() == :colon
            self.advance_token()
            type_name = self.parse_type_annotation()
            more = false if @failed
          end
          types.push(type_name)
          self.skip_newlines()
          if !@failed && @current.kind() == :comma
            self.advance_token()
            self.skip_newlines()
            more = @current.kind() != :right_paren
          else
            more = false
          end
        end
      end
    end
    [names, types]
  end

  def parse_type_annotation()
    members = []
    parsing = true
    while parsing && !@failed
      if @current.kind() != :identifier
        self.fail("expected type annotation")
      else
        name = self.token_text(@current)
        self.advance_token()
        while !@failed && @current.kind() == :double_colon
          self.advance_token()
          if @current.kind() != :identifier
            self.fail("type name is too long")
          else
            name = name + "::" + self.token_text(@current)
            self.advance_token()
          end
        end
        index = 0
        while index < members.length()
          self.fail("duplicate type in union") if members[index][0] == name
          index = index + 1
        end
        if members.length() == 8
          self.fail("too many types in union")
        else
          argument = nil
          second_argument = nil
          callable_arity = -1
          callable_return = nil
          callable_parameters = nil
          if !@failed && @current.kind() == :left_bracket
            if name == "Callable"
              callable = self.parse_callable_arguments()
              callable_arity = callable[0]
              callable_return = callable[1]
              callable_parameters = callable[2]
            elsif name != "Array" && name != "Hash"
              self.fail("this type does not accept arguments")
            else
              self.advance_token()
              self.skip_newlines()
              argument = self.parse_type_annotation()
              self.skip_newlines()
              if name == "Hash"
                if @current.kind() != :comma
                  self.fail("expected ',' between Hash key and value types")
                else
                  self.advance_token()
                  self.skip_newlines()
                  second_argument = self.parse_type_annotation()
                  self.skip_newlines()
                end
              end
              if !@failed && @current.kind() != :right_bracket
                self.fail("expected ']' after collection type arguments")
              else
                self.advance_token() unless @failed
              end
            end
          elsif name == "Array" || name == "Hash"
            # Unparameterized collections remain valid dynamic contracts.
          end
          if !@failed
            member = [name, argument, second_argument, callable_arity,
                      callable_return, callable_parameters]
            members.push(member)
          end
        end
        if !@failed && @current.kind() == :pipe
          self.advance_token()
        else
          parsing = false
        end
      end
    end
    members
  end

  def parse_callable_arguments()
    self.advance_token()
    self.skip_newlines()
    arity = -1
    return_type = nil
    parameter_types = nil
    if @current.kind() == :integer
      text = self.token_text(@current)
      arity = text.to_i()
      if arity > 16
        self.fail("Callable arity cannot exceed 16")
      end
      self.advance_token() unless @failed
    elsif @current.kind() == :left_bracket
      self.advance_token()
      self.skip_newlines()
      parameter_types = []
      while !@failed && @current.kind() != :right_bracket
        if parameter_types.length() == 16
          self.fail("Callable cannot exceed 16 parameters")
        else
          parameter_types.push(self.parse_type_annotation())
          self.skip_newlines()
          if @current.kind() == :comma
            self.advance_token()
            self.skip_newlines()
          else
            break
          end
        end
      end
      if !@failed && @current.kind() != :right_bracket
        self.fail("expected ']' after Callable parameters")
      else
        self.advance_token() unless @failed
      end
      arity = parameter_types.length() unless @failed
    else
      self.fail("expected Callable arity or parameter list")
    end
    if !@failed && @current.kind() == :comma
      self.advance_token()
      self.skip_newlines()
      return_type = self.parse_type_annotation()
    end
    self.skip_newlines()
    if !@failed && @current.kind() != :right_bracket
      self.fail("expected ']' after collection type arguments")
    else
      self.advance_token() unless @failed
    end
    [arity, return_type, parameter_types]
  end

  def resolve_type_name(name)
    return Type::INT if name == "Int"
    return Type::FLOAT if name == "Float"
    return Type::STRING if name == "String"
    return Type::BOOL if name == "Bool"
    return Type::NIL if name == "Nil"
    return Type::ARRAY if name == "Array"
    return Type::HASH if name == "Hash"
    return Type::CALLABLE if name == "Callable"
    variable = 0
    while variable < @current_type_variables.length()
      return 96 + variable if @current_type_variables[variable] == name
      variable = variable + 1
    end
    interface_entry = self.find_interface(name)
    if interface_entry != nil
      return 128 + interface_entry[1]
    end
    class_entry = self.find_class(name)
    if class_entry != nil
      return Type::CLASS_BASE + class_entry[1]
    end
    self.fail("unknown type annotation")
    0
  end

  def compile_interface()
    self.advance_token()
    if @function_nesting_depth != 0 || @current_class_index != nil
      self.fail("interfaces must be declared at top level")
      return 0
    end
    if @current.kind() != :identifier
      self.fail("expected valid interface name")
      return 0
    end
    name = self.token_text(@current)
    if @current_module_name != nil
      name = @current_module_name + "::" + name
    end
    if self.find_interface(name) != nil || self.find_class(name) != nil
      self.fail("type name is already defined")
      return 0
    end
    index = 0
    while index < @modules.length()
      self.fail("type name is already defined") if @modules[index][0] == name
      index = index + 1
    end
    return 0 if @failed
    interface_index = @builder.declare_interface(name)
    @interfaces.push([name, interface_index])
    self.advance_token()
    if @current.kind() == :less
      self.advance_token()
      self.skip_newlines()
      parsing_bases = true
      while parsing_bases && !@failed
        if @current.kind() != :identifier
          self.fail("expected base interface name after '<'")
        else
          base_name = self.token_text(@current)
          base = self.find_interface(base_name)
          if base == nil && @current_module_name != nil
            base = self.find_interface(@current_module_name + "::" + base_name)
          end
          if base == nil
            self.fail("undefined base interface")
          else
            @builder.inherit_interface(interface_index, base[1])
            self.advance_token()
            if @current.kind() == :comma
              self.advance_token()
              self.skip_newlines()
            else
              parsing_bases = false
            end
          end
        end
      end
    end
    return 0 unless self.consume_block_start()
    while !@failed && @current.kind() != :end
      self.compile_interface_method(interface_index)
      self.skip_newlines()
    end
    if @current.kind() == :end
      self.advance_token()
    else
      self.fail("expected 'end' after interface")
    end
    self.allocate_register()
  end

  def compile_interface_method(interface_index)
    if @current.kind() != :def
      self.fail("expected method signature in interface")
      return
    end
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected interface method name")
      return
    end
    name = self.token_text(@current)
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after interface method")
      return
    end
    self.advance_token()
    parsed = self.parse_parameter_names()
    if @current.kind() != :right_paren
      self.fail("expected ')' after interface parameters")
      return
    end
    self.advance_token()
    return_type = nil
    if @current.kind() == :arrow
      self.advance_token()
      return_type = self.parse_type_annotation()
    end
    parameter_sets = []
    index = 0
    while index < parsed[1].length() && !@failed
      annotation = parsed[1][index]
      if annotation == nil
        parameter_sets.push(-1)
      else
        parameter_sets.push(self.declare_annotation(annotation))
      end
      index = index + 1
    end
    return_set = if return_type == nil
      -1
    else
      self.declare_annotation(return_type)
    end
    if !@failed
      @builder.declare_interface_method(interface_index, name,
        parsed[0].length(), parameter_sets, return_set)
    end
  end

  def declare_annotation(type_annotation)
    descriptors = []
    index = 0
    while index < type_annotation.length() && !@failed
      member = type_annotation[index]
      type_id = self.resolve_type_name(member[0])
      argument_set = if member[1] == nil
        -1
      else
        self.declare_annotation(member[1])
      end
      second_argument_set = if member[2] == nil
        -1
      else
        self.declare_annotation(member[2])
      end
      callable_return_set = if member[4] == nil
        -1
      else
        self.declare_annotation(member[4])
      end
      callable_parameter_sets = []
      if member[5] != nil
        parameter = 0
        while parameter < member[5].length()
          callable_parameter_sets.push(self.declare_annotation(member[5][parameter]))
          parameter = parameter + 1
        end
      end
      descriptors.push([type_id, argument_set, second_argument_set, member[3],
                        callable_return_set, callable_parameter_sets])
      index = index + 1
    end
    return 0 if @failed
    @builder.declare_type_set(@current_function_index, descriptors)
  end

  def emit_type_check(reg, type_annotation)
    fact = self.type_fact(reg)
    if fact != nil && self.annotation_accepts_type?(type_annotation, fact)
      return self.declare_annotation(type_annotation)
    end
    set_index = self.declare_annotation(type_annotation)
    return if @failed
    self.emit_instruction2(Opcode::CHECK_TYPE, reg, set_index)
    set_index
  end

  def type_fact(reg)
    index = @type_facts.length() - 1
    while index >= 0
      return @type_facts[index][1] if @type_facts[index][0] == reg
      index = index - 1
    end
    nil
  end

  def set_type_fact(reg, type_id)
    @type_facts.push([reg, type_id])
  end

  def copy_type_facts()
    copy = []
    index = 0
    while index < @type_facts.length()
      copy.push(@type_facts[index])
      index = index + 1
    end
    copy
  end

  def type_fact_in(facts, reg)
    index = facts.length() - 1
    while index >= 0
      return facts[index][1] if facts[index][0] == reg
      index = index - 1
    end
    nil
  end

  def merge_local_type_facts(original, then_facts, else_facts)
    @type_facts = original
    index = 0
    while index < @locals.length()
      local = @locals[index]
      unless local[2]
        then_fact = self.type_fact_in(then_facts, local[1])
        else_fact = self.type_fact_in(else_facts, local[1])
        if then_fact != nil && then_fact == else_fact
          self.set_type_fact(local[1], then_fact)
        end
      end
      index = index + 1
    end
  end

  def annotation_accepts_type?(annotation, type_id)
    index = 0
    while index < annotation.length()
      return true if self.resolve_type_name(annotation[index][0]) == type_id
      index = index + 1
    end
    false
  end

  def declared_type(reg)
    index = @declared_types.length() - 1
    while index >= 0
      return @declared_types[index][1] if @declared_types[index][0] == reg
      index = index - 1
    end
    nil
  end

  def non_nil_single_type(annotation)
    found = nil
    index = 0
    while index < annotation.length()
      type_id = self.resolve_type_name(annotation[index][0])
      if type_id != Type::NIL
        return nil if found != nil
        found = type_id
      end
      index = index + 1
    end
    found
  end

  def remaining_single_type(annotation, excluded)
    found = nil
    index = 0
    while index < annotation.length()
      type_id = self.resolve_type_name(annotation[index][0])
      if type_id != excluded
        return nil if found != nil
        found = type_id
      end
      index = index + 1
    end
    found
  end

  def annotation_single_type(annotation)
    return nil if annotation == nil || annotation.length() != 1
    self.resolve_type_name(annotation[0][0])
  end

  def binary_result_fact(operator, left, right)
    left_fact = self.type_fact(left)
    right_fact = self.type_fact(right)
    return Type::INT if left_fact == Type::INT && right_fact == Type::INT
    left_numeric = left_fact == Type::INT || left_fact == Type::FLOAT
    right_numeric = right_fact == Type::INT || right_fact == Type::FLOAT
    if left_numeric && right_numeric && (left_fact == Type::FLOAT || right_fact == Type::FLOAT)
      return Type::FLOAT
    end
    if operator == :plus && left_fact == Type::STRING && right_fact == Type::STRING
      return Type::STRING
    end
    nil
  end

  def apply_result_join(destination, left, right)
    left_fact = self.type_fact(left)
    right_fact = self.type_fact(right)
    self.set_type_fact(destination, left_fact) if left_fact != nil && left_fact == right_fact
    left_declaration = self.declared_type(left)
    right_declaration = self.declared_type(right)
    if left_declaration != nil && left_declaration == right_declaration
      @declared_types.push([destination, left_declaration])
    end
  end

  def annotation_with_nil(annotation)
    result = []
    has_nil = false
    index = 0
    while index < annotation.length()
      result.push(annotation[index])
      has_nil = true if self.resolve_type_name(annotation[index][0]) == Type::NIL
      index = index + 1
    end
    result.push(["Nil", nil, nil, -1, nil, nil]) unless has_nil
    result
  end

  def homogeneous_element_annotation(elements)
    return nil if elements.length() == 0
    fact = self.type_fact(elements[0])
    return nil if fact == nil
    index = 1
    while index < elements.length()
      return nil if self.type_fact(elements[index]) != fact
      index = index + 1
    end
    name = if fact == Type::INT
      "Int"
    elsif fact == Type::FLOAT
      "Float"
    elsif fact == Type::STRING
      "String"
    elsif fact == Type::BOOL
      "Bool"
    elsif fact == Type::NIL
      "Nil"
    else
      nil
    end
    return nil if name == nil
    [[name, nil, nil, -1, nil, nil]]
  end

  # Switches compiler state into the new function, compiles its body,
  # and restores the outer state afterward -- the part of
  # compile_definition that's identical for a top-level function and a
  # nested closure alike (the only difference between the two is what
  # happens with the result *after* this returns, handled by
  # compile_definition/emit_closure).
  def compile_function_body(function_index, parameter_names, parameter_types,
                            return_type, enclosing_locals)
    outer_locals = @locals
    outer_loops = @loops
    outer_next_register = @next_register
    outer_code_count = @code_count
    outer_function_index = @current_function_index
    outer_enclosing_locals = @enclosing_locals
    outer_return_type = @current_return_type
    outer_type_facts = @type_facts
    outer_declared_types = @declared_types

    @locals = []
    @loops = []
    @next_register = 0
    @code_count = 0
    @current_function_index = function_index
    @enclosing_locals = enclosing_locals
    @current_return_type = return_type
    @type_facts = []
    @declared_types = []
    @function_nesting_depth = @function_nesting_depth + 1

    index = 0
    while index < parameter_names.length()
      register = self.define_local(parameter_names[index])
      type_name = parameter_types[index]
      if type_name != nil
        @declared_types.push([register, type_name])
        set_index = self.emit_type_check(register, type_name)
        @builder.set_parameter_type(function_index, index, set_index)
      end
      index = index + 1
    end

    # GET_CAPTURE_CELL for every enclosing local not shadowed by one of
    # this function's own parameters -- from here on, referencing that
    # name inside the body reads/writes this cell (via the ordinary
    # find_local/read_local/compile_assignment paths, no special-casing
    # needed) rather than any register in the outer function directly.
    index = 0
    while index < enclosing_locals.length()
      entry = enclosing_locals[index]
      if self.find_local(entry[0]) == nil
        cell = self.allocate_register()
        self.emit_instruction2(Opcode::GET_CAPTURE_CELL, cell, index)
        @locals.push([entry[0], cell, true])
      end
      index = index + 1
    end

    endless = @current.kind() == :equal
    if endless
      self.advance_token()
      postfix = self.postfix_modifier_ahead()
      has_postfix = postfix == :if || postfix == :unless
      postfix_result = self.allocate_register() if has_postfix
      condition_jump = self.emit_jump(Opcode::JUMP, 0) if has_postfix
      body_start = @code_count
      body_result = self.parse_expression()
      if has_postfix
        if @current.kind() != postfix
          self.fail("expected postfix condition")
        else
          self.advance_token()
          self.emit_instruction2(Opcode::MOVE, postfix_result, body_result)
          body_exit = self.emit_jump(Opcode::JUMP, 0)
          condition_start = @code_count
          condition = self.parse_expression()
          body_jump = self.emit_jump(if postfix == :if
            Opcode::JUMP_IF_TRUE
          else
            Opcode::JUMP_IF_FALSE
          end, condition)
          self.emit_instruction1(Opcode::NIL, postfix_result)
          self.patch_jump(condition_jump, condition_start)
          self.patch_jump(body_exit, @code_count)
          self.patch_jump(body_jump, body_start)
          body_result = postfix_result
        end
      end
      if return_type != nil
        set_index = self.emit_type_check(body_result, return_type)
        @builder.set_return_type(function_index, set_index)
      end
      self.emit_instruction1(Opcode::RETURN, body_result)
    elsif self.consume_block_start()
      body_result = self.compile_sequence()
      if return_type != nil
        set_index = self.emit_type_check(body_result, return_type)
        @builder.set_return_type(function_index, set_index)
      end
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
    @enclosing_locals = outer_enclosing_locals
    @current_return_type = outer_return_type
    @type_facts = outer_type_facts
    @declared_types = outer_declared_types
    @function_nesting_depth = @function_nesting_depth - 1
  end

  # BOX_LOCAL runs back in the OUTER scope, after the nested body is
  # fully compiled: converts each captured outer register from a plain
  # value into a Cell in place (idempotent -- a no-op if it's already
  # one, e.g. this same local was captured by an earlier nested def
  # too), and marks the outer entry `captured` so every subsequent
  # read/write of that name in the outer function also goes through
  # GET_CELL/SET_CELL from here on, matching compiler.c's own
  # once-boxed-always-boxed semantics exactly.
  def emit_closure(function_index, enclosing_locals, name)
    capture_registers = []
    index = 0
    while index < enclosing_locals.length()
      entry = enclosing_locals[index]
      self.emit_instruction1(Opcode::BOX_LOCAL, entry[1])
      entry[2] = true
      capture_registers.push(entry[1])
      index = index + 1
    end
    result = self.allocate_register()
    self.emit_byte(Opcode::CLOSURE)
    self.emit_byte(result)
    self.emit_byte(function_index)
    self.emit_byte(capture_registers.length())
    index = 0
    while index < capture_registers.length()
      self.emit_byte(capture_registers[index])
      index = index + 1
    end
    @locals.push([name, result, false])
    result
  end

  # `class Name ... end` or `class Name < Superclass ... end` --
  # top-level only (no nested classes, no class declared inside a
  # function/method body: a much narrower scope cut than compiler.c's
  # own class declarations, which have no such restriction -- confirmed
  # by testing directly against the real compiler, not assumed).
  def compile_class()
    self.advance_token()
    if @function_nesting_depth != 0 || @current_class_index != nil
      self.fail("classes must be declared at the top level")
      return 0
    end
    if @current.kind() != :identifier
      self.fail("expected valid class name")
      return 0
    end
    name = self.token_text(@current)
    if @current_module_name != nil
      name = @current_module_name + "::" + name
    end
    if self.find_class(name) != nil
      self.fail("class is already defined")
      return 0
    end
    if self.find_interface(name) != nil
      self.fail("type name is already defined")
      return 0
    end
    index = 0
    while index < @modules.length()
      self.fail("type name is already defined") if @modules[index][0] == name
      index = index + 1
    end
    return 0 if @failed
    self.advance_token()
    superclass_index = self.parse_optional_superclass()
    return 0 if @failed
    class_index = @builder.declare_class(name, if superclass_index == nil
      -1
    else
      superclass_index
    end)
    @classes.push([name, class_index, superclass_index])
    @current_class_index = class_index
    @current_class_superclass_index = superclass_index
    @current_class_method_names = []

    if !self.consume_block_start()
      @current_class_index = nil
      @current_class_superclass_index = nil
      return 0
    end
    while !@failed && @current.kind() != :end
      if @current.kind() == :def
        self.compile_method()
      elsif @current.kind() == :include
        self.advance_token()
        if @current.kind() != :identifier
          self.fail("expected module name after 'include'")
        else
          module_name = self.consume_qualified_module_name()
          module_index = nil
          index = 0
          while index < @modules.length()
            module_index = @modules[index][1] if @modules[index][0] == module_name
            index = index + 1
          end
          if module_index == nil
            self.fail("undefined module")
          else
            @builder.include_module(@current_class_index, module_index)
          end
        end
      else
        self.fail("expected method definition or include in class")
      end
      self.skip_newlines() if @current.kind() == :newline
    end
    if @current.kind() != :end
      self.fail("expected method definition or include in class") unless @failed
      @current_class_index = nil
      @current_class_superclass_index = nil
      return 0
    end
    self.advance_token()
    @current_class_index = nil
    @current_class_superclass_index = nil
    # Sole-writer fresh register; run_chunk's zero-init already covers
    # nil, matching compile_class.c's own return value exactly.
    self.allocate_register()
  end

  # Returns the superclass's class_index, or nil if there's no `<
  # Superclass` clause at all. Doesn't itself call declare_class --
  # compile_class still needs the -1-vs-index translation for that call,
  # so it stays the caller's job.
  def parse_optional_superclass()
    return nil unless @current.kind() == :less
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected superclass name after '<'")
      return nil
    end
    superclass_name = self.token_text(@current)
    entry = self.find_class(superclass_name)
    if entry == nil && @current_module_name != nil
      entry = self.find_class(@current_module_name + "::" + superclass_name)
    end
    if entry == nil
      self.fail("undefined superclass")
      return nil
    end
    self.advance_token()
    entry[1]
  end

  def compile_module()
    self.advance_token()
    if @function_nesting_depth != 0 || @current_class_index != nil
      self.fail("modules must be declared at top level")
      return 0
    end
    if @current.kind() != :identifier
      self.fail("expected valid module name")
      return 0
    end
    local_name = self.token_text(@current)
    name = if @current_module_name == nil
      local_name
    else
      @current_module_name + "::" + local_name
    end
    index = 0
    while index < @modules.length()
      self.fail("module name is already defined") if @modules[index][0] == name
      index = index + 1
    end
    self.fail("module name is already defined") if self.find_class(name) != nil || self.find_interface(name) != nil
    return 0 if @failed
    module_index = @builder.declare_module(name)
    module_entry = @modules.length()
    @modules.push([name, module_index, [], [], [false], [false], [], [], @current_module_name])
    outer_module_index = @current_module_index
    outer_module_name = @current_module_name
    outer_module_entry = @current_module_entry
    @current_module_index = module_index
    @current_module_name = name
    @current_module_entry = module_entry
    self.advance_token()
    if self.consume_block_start()
      while !@failed && @current.kind() != :end
        if @current.kind() == :def
          self.compile_method()
        elsif @current.kind() == :interface
          self.compile_interface()
        elsif @current.kind() == :class
          self.compile_class()
        elsif @current.kind() == :module
          self.compile_module()
        elsif @current.kind() == :module_function
          self.compile_module_function()
        elsif @current.kind() == :include || @current.kind() == :private || @current.kind() == :public
          self.compile_module_include()
        elsif self.assignment_ahead?()
          self.compile_module_constant(name)
        else
          self.fail("expected module constant or method definition")
        end
        self.skip_newlines() if @current.kind() == :newline
      end
      self.advance_token() if @current.kind() == :end
    end
    @current_module_index = outer_module_index
    @current_module_name = outer_module_name
    @current_module_entry = outer_module_entry
    self.allocate_register()
  end

  def compile_module_function()
    self.advance_token()
    parenthesized = @current.kind() == :left_paren
    self.advance_token() if parenthesized
    if @current.kind() == :identifier
      more = true
      while more && !@failed
        method_name = self.token_text(@current)
        self.advance_token()
        if @current.kind() == :equal
          method_name = method_name + "="
          self.advance_token()
        end
        found = false
        descriptor = nil
        index = 0
        while index < @modules[@current_module_entry][3].length()
          if @modules[@current_module_entry][3][index] == method_name
            found = true
            descriptor = @modules[@current_module_entry][6][index]
          end
          index = index + 1
        end
        if found
          if descriptor[3]
            self.fail("stateful module method cannot become a module_function")
          else
            @builder.export_module_method(@current_module_index, method_name)
            @modules[@current_module_entry][7].push(descriptor)
          end
        else
          self.fail("module_function target is not defined here")
        end
        if @current.kind() == :comma
          self.advance_token()
        else
          more = false
        end
      end
      if parenthesized
        if @current.kind() == :right_paren
          self.advance_token()
        else
          self.fail("expected ')' after module_function targets")
        end
      end
    elsif parenthesized
      self.fail("expected method in module_function list")
    else
      mode = @modules[@current_module_entry][5]
      mode[0] = true
    end
  end

  def compile_module_constant(module_name)
    constant_name = self.token_text(@current)
    first = constant_name.slice(0, 1)
    if first == "_" || first != first.upcase()
      self.fail("module constants must begin with an uppercase letter")
      return
    end
    qualified = module_name + "::" + constant_name
    constant_index = nil
    index = 0
    while index < @modules[@current_module_entry][2].length()
      constant_index = 0 if @modules[@current_module_entry][2][index][0] == constant_name
      index = index + 1
    end
    if constant_index != nil
      self.fail("constant is already defined")
      return
    end
    constant_index = @builder.declare_namespace_constant(qualified)
    @modules[@current_module_entry][2].push([constant_name, constant_index])
    self.advance_token()
    self.advance_token()
    value = self.parse_expression()
    self.emit_instruction2(49, constant_index, value)
    if @current.kind() == :if || @current.kind() == :unless
      self.fail("expected definition or include in module")
    end
  end

  def compile_module_include()
    if @current.kind() == :private || @current.kind() == :public
      private_mode = @current.kind() == :private
      self.advance_token()
      parenthesized = @current.kind() == :left_paren
      self.advance_token() if parenthesized
      if @current.kind() == :identifier
        more = true
        while more && !@failed
          name = self.token_text(@current)
          found = false
          index = 0
          while index < @modules[@current_module_entry][3].length()
            found = true if @modules[@current_module_entry][3][index] == name
            index = index + 1
          end
          if found
            @builder.set_module_method_visibility(@current_module_index, name, private_mode)
          else
            self.fail("visibility target is not defined here")
          end
          self.advance_token()
          if @current.kind() == :comma
            self.advance_token()
          else
            more = false
          end
        end
        if parenthesized
          if @current.kind() == :right_paren
            self.advance_token()
          else
            self.fail("expected ')' after visibility targets")
          end
        end
      elsif parenthesized
        self.fail("expected method name in visibility list")
      else
        mode = @modules[@current_module_entry][4]
        mode[0] = private_mode
      end
      return
    end
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected module name after 'include'")
      return
    end
    name = self.consume_qualified_module_name()
    included = nil
    index = 0
    while index < @modules.length()
      included = @modules[index][1] if @modules[index][0] == name
      index = index + 1
    end
    if included == nil && @modules[@current_module_entry][8] != nil
      name = @modules[@current_module_entry][8] + "::" + name
      index = 0
      while index < @modules.length()
        included = @modules[index][1] if @modules[index][0] == name
        index = index + 1
      end
    end
    if included == nil
      self.fail("undefined module")
    elsif included == @current_module_index
      self.fail("module cannot include itself")
    else
      @builder.include_module_in_module(@current_module_index, included)
    end
  end

  def consume_qualified_module_name()
    name = self.token_text(@current)
    self.advance_token()
    while !@failed && @current.kind() == :double_colon
      self.advance_token()
      if @current.kind() != :identifier
        self.fail("invalid qualified module name")
      else
        name = name + "::" + self.token_text(@current)
        self.advance_token()
      end
    end
    name
  end

  # An instance method: register 0 is always `self` (allocated before
  # any user-declared parameter, exactly mirroring compile_definition.c's
  # own class/module branch), and the compiled function is registered
  # into the class via declare_method rather than @functions -- resolved
  # later at INVOKE time by the VM against the receiver's runtime class,
  # not by this parser at compile time the way a direct top-level call
  # is. No closures inside a method body this round (methods don't
  # participate in @function_nesting_depth at all).
  def compile_method()
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected function name after 'def'")
      return
    end
    name = self.token_text(@current)
    self.advance_token()
    if @current.kind() == :equal
      name = name + "="
      self.advance_token()
    end
    if @current_module_index != nil && @current_class_index == nil
      index = 0
      while index < @modules[@current_module_entry][3].length()
        self.fail("method is already defined") if @modules[@current_module_entry][3][index] == name
        index = index + 1
      end
      return if @failed
    else
      index = 0
      while index < @current_class_method_names.length()
        self.fail("method is already defined") if @current_class_method_names[index] == name
        index = index + 1
      end
      if @failed
        return
      end
    end
    if @current.kind() != :left_paren
      self.fail("expected '(' after function name")
      return
    end
    self.advance_token()
    parsed_parameters = self.parse_parameter_names()
    return if @failed
    if @current.kind() != :right_paren
      self.fail("expected ')' after parameters")
      return
    end
    self.advance_token()
    parameter_names = parsed_parameters[0]
    parameter_types = parsed_parameters[1]
    return_type = nil
    if @current.kind() == :arrow
      self.advance_token()
      return_type = self.parse_type_annotation()
    end
    return if @failed

    arity = parameter_names.length()
    function_index = @builder.declare_function(name, arity + 1, arity + 1)
    outer_method_name = @current_method_name
    @current_method_name = name
    @current_method_uses_state = false
    self.compile_method_body(function_index, parameter_names, parameter_types, return_type)
    @current_method_name = outer_method_name
    if @current_module_index != nil && @current_class_index == nil
      self.register_module_method(name, function_index, arity)
    else
      @current_class_method_names.push(name)
      @builder.declare_method(@current_class_index, name, function_index, arity, arity, false)
    end
    @current_method_uses_state = false
  end

  def register_module_method(name, function_index, arity)
    @modules[@current_module_entry][3].push(name)
    descriptor = [name, function_index, arity, @current_method_uses_state]
    @modules[@current_module_entry][6].push(descriptor)
    @builder.declare_module_method(@current_module_index, name, function_index,
      arity, arity, @modules[@current_module_entry][4][0])
    if @modules[@current_module_entry][5][0]
      if descriptor[3]
        self.fail("stateful method cannot use module_function mode")
      else
        @builder.export_module_method(@current_module_index, name)
        @modules[@current_module_entry][7].push(descriptor)
      end
    end
  end

  def compile_method_body(function_index, parameter_names, parameter_types, return_type)
    outer_locals = @locals
    outer_loops = @loops
    outer_next_register = @next_register
    outer_code_count = @code_count
    outer_function_index = @current_function_index
    outer_return_type = @current_return_type
    outer_type_facts = @type_facts
    outer_declared_types = @declared_types

    @locals = []
    @loops = []
    @next_register = 0
    @code_count = 0
    @current_function_index = function_index
    @current_return_type = return_type
    @type_facts = []
    @declared_types = []
    self.allocate_register()

    index = 0
    while index < parameter_names.length()
      register = self.define_local(parameter_names[index])
      type_name = parameter_types[index]
      if type_name != nil
        @declared_types.push([register, type_name])
        set_index = self.emit_type_check(register, type_name)
        @builder.set_parameter_type(function_index, index + 1, set_index) unless @failed
      end
      index = index + 1
    end

    endless = @current.kind() == :equal
    if endless
      self.advance_token()
      postfix = self.postfix_modifier_ahead()
      has_postfix = postfix == :if || postfix == :unless
      postfix_result = self.allocate_register() if has_postfix
      condition_jump = self.emit_jump(Opcode::JUMP, 0) if has_postfix
      body_start = @code_count
      body_result = self.parse_expression()
      if has_postfix
        if @current.kind() != postfix
          self.fail("expected postfix condition")
        else
          self.advance_token()
          self.emit_instruction2(Opcode::MOVE, postfix_result, body_result)
          body_exit = self.emit_jump(Opcode::JUMP, 0)
          condition_start = @code_count
          condition = self.parse_expression()
          body_jump = self.emit_jump(if postfix == :if
            Opcode::JUMP_IF_TRUE
          else
            Opcode::JUMP_IF_FALSE
          end, condition)
          self.emit_instruction1(Opcode::NIL, postfix_result)
          self.patch_jump(condition_jump, condition_start)
          self.patch_jump(body_exit, @code_count)
          self.patch_jump(body_jump, body_start)
          body_result = postfix_result
        end
      end
      if return_type != nil
        set_index = self.emit_type_check(body_result, return_type)
        @builder.set_return_type(function_index, set_index) unless @failed
      end
      self.emit_instruction1(Opcode::RETURN, body_result)
    elsif self.consume_block_start()
      body_result = self.compile_sequence()
      if return_type != nil
        set_index = self.emit_type_check(body_result, return_type)
        @builder.set_return_type(function_index, set_index) unless @failed
      end
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
    @current_return_type = outer_return_type
    @type_facts = outer_type_facts
    @declared_types = outer_declared_types
  end

  # Shared by compile_call/compile_closure_call: parses `(arg, arg, ...)`
  # (the opening '(' already consumed by the caller) and moves each
  # argument's value register into a fresh, contiguous block starting at
  # a newly allocated base register -- the layout every call-family
  # opcode (CALL/CALL_CLOSURE) expects. Returns [argument_base, count],
  # or nil on a parse failure (caller should return 0 in that case).
  def parse_call_arguments()
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
      return nil
    end
    self.advance_token()
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
    [argument_base, arguments.length()]
  end

  # Direct calls to a top-level function support keyword arguments
  # (`f(y: 2, x: 1)`), unlike closure calls/NEW/INVOKE (which stay
  # positional-only, via parse_call_arguments) -- compiler.c scopes
  # keyword arguments the same way, since only a direct call has
  # compile-time, non-polymorphic access to the target's exact
  # parameter names. No defaults exist in this port (see this file's
  # header comment), so unlike compiler.c's own version, every
  # declared slot must always be filled -- there's no partial-call
  # case to allow.
  def compile_call(name)
    function_entry = self.find_function(name)
    if function_entry == nil
      self.fail("undefined function")
      return 0
    end
    type_arguments = []
    if @current.kind() == :left_bracket
      self.advance_token()
      self.skip_newlines()
      while !@failed && @current.kind() != :right_bracket
        if type_arguments.length() == 8
          self.fail("too many generic arguments")
        else
          annotation = self.parse_type_annotation()
          type_arguments.push(self.declare_annotation(annotation))
          self.skip_newlines()
          if @current.kind() == :comma
            self.advance_token()
            self.skip_newlines()
          else
            break
          end
        end
      end
      if !@failed && @current.kind() != :right_bracket
        self.fail("expected ']' after generic arguments")
      else
        self.advance_token() unless @failed
      end
      if !@failed && type_arguments.length() != function_entry[4]
        self.fail("wrong number of generic arguments")
      end
      if !@failed && @current.kind() != :left_paren
        self.fail("expected '(' after generic arguments")
      end
    elsif function_entry[4] > 0
      # Omitted arguments use the VM's ordinary runtime inference.
    end
    return 0 if @failed
    self.advance_token()
    self.skip_newlines()
    slot_values = self.parse_keyword_call_arguments(function_entry)
    return 0 if slot_values == nil
    arity = function_entry[2]
    argument_base = self.allocate_register()
    i = 1
    while i < arity
      self.allocate_register()
      i = i + 1
    end
    i = 0
    while i < arity
      self.emit_instruction2(Opcode::MOVE, argument_base + i, slot_values[i])
      i = i + 1
    end
    destination = self.allocate_register()
    if type_arguments.length() == 0
      self.emit_byte(Opcode::CALL)
    else
      self.emit_byte(Opcode::CALL_TYPED)
    end
    self.emit_byte(destination)
    self.emit_byte(function_entry[1])
    self.emit_byte(argument_base)
    self.emit_byte(arity)
    if type_arguments.length() > 0
      self.emit_byte(type_arguments.length())
      i = 0
      while i < type_arguments.length()
        self.emit_byte(type_arguments[i])
        i = i + 1
      end
    end
    return_fact = if function_entry[4] == 0
      self.annotation_single_type(function_entry[5])
    else
      nil
    end
    self.set_type_fact(destination, return_fact) if return_fact != nil
    if function_entry[4] == 0 && function_entry[5] != nil
      @declared_types.push([destination, function_entry[5]])
    end
    destination
  end

  def keyword_argument_ahead?()
    return false if @current.kind() != :identifier
    lookahead = @lexer.clone()
    lookahead.next_token().kind() == :colon
  end

  def find_parameter_slot(parameter_names, name)
    index = 0
    result = -1
    while index < parameter_names.length() && result == -1
      result = index if parameter_names[index] == name
      index = index + 1
    end
    result
  end

  # Returns an Array of `arity` value registers, one per declared
  # parameter slot (in declaration order, regardless of the order
  # arguments were written at the call site), or nil on a parse
  # failure. Mirrors compiler.c's own slot_registers/slot_filled
  # tracking: a keyword fills its named slot directly; a positional
  # argument fills the next not-yet-seen slot in order; a positional
  # argument can't follow a keyword one.
  def parse_keyword_call_arguments(function_entry)
    arity = function_entry[2]
    parameter_names = function_entry[3]
    slot_values = []
    slot_filled = []
    index = 0
    while index < arity
      slot_values.push(nil)
      slot_filled.push(false)
      index = index + 1
    end
    next_positional_slot = 0
    seen_keyword = false
    if @current.kind() != :right_paren
      more = true
      while more
        slot = -1
        if self.keyword_argument_ahead?()
          keyword_name = self.token_text(@current)
          self.advance_token()
          self.advance_token()
          slot = self.find_parameter_slot(parameter_names, keyword_name)
          if slot == -1
            self.fail("no parameter with this name")
            return nil
          end
          seen_keyword = true
        else
          if seen_keyword
            self.fail("positional argument cannot follow a keyword argument")
            return nil
          end
          if next_positional_slot >= arity
            self.fail("too many call arguments")
            return nil
          end
          slot = next_positional_slot
          next_positional_slot = next_positional_slot + 1
        end
        if slot_filled[slot]
          self.fail("multiple values for the same argument")
          return nil
        end
        slot_values[slot] = self.parse_expression()
        slot_filled[slot] = true
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
      return nil
    end
    self.advance_token()
    index = 0
    while index < arity
      if !slot_filled[index]
        self.fail("missing argument")
        return nil
      end
      index = index + 1
    end
    slot_values
  end

  # Calling a closure-valued local (see compile_definition's nested-`def`
  # branch) -- unlike compile_call, the target's arity isn't known at
  # compile time (only the runtime Closure value's own function_index
  # carries it), so there's no argument-count check here; the VM raises
  # an ordinary ArityError at CALL_CLOSURE time instead, matching
  # compiler.c's own parse_call CALL_CLOSURE branch exactly.
  def compile_closure_call(local)
    callable = self.read_local(local)
    self.advance_token()
    parsed = self.parse_call_arguments()
    return 0 if parsed == nil
    destination = self.allocate_register()
    self.emit_byte(Opcode::CALL_CLOSURE)
    self.emit_byte(destination)
    self.emit_byte(callable)
    self.emit_byte(parsed[0])
    self.emit_byte(parsed[1])
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
    return false if @current.kind() != :identifier && @current.kind() != :instance_variable
    lookahead = @lexer.clone()
    lookahead.next_token().kind() == :equal
  end

  def index_assignment_ahead?()
    return false if @current.kind() != :identifier
    lookahead = @lexer.clone()
    return false if lookahead.next_token().kind() != :left_bracket
    depth = 1
    while depth > 0
      kind = lookahead.next_token().kind()
      return false if kind == :eof || kind == :newline
      depth = depth + 1 if kind == :left_bracket
      depth = depth - 1 if kind == :right_bracket
    end
    lookahead.next_token().kind() == :equal
  end

  def compile_index_assignment()
    name = self.token_text(@current)
    local = self.find_local(name)
    if local == nil
      self.fail("undefined local variable")
      return 0
    end
    receiver = self.read_local(local)
    self.advance_token()
    self.advance_token()
    index = self.parse_expression()
    if @current.kind() != :right_bracket
      self.fail("expected ']' after assignment index")
      return 0
    end
    self.advance_token()
    if @current.kind() != :equal
      self.fail("expected '=' after indexed target")
      return 0
    end
    self.advance_token()
    value = self.parse_expression()
    self.emit_instruction3(Opcode::INDEX_SET, receiver, index, value)
    value
  end

  def compile_assignment()
    instance_variable = @current.kind() == :instance_variable
    token = @current
    self.advance_token()
    self.advance_token()
    value = self.parse_expression()
    return self.compile_ivar_write(token, value) if instance_variable
    name = self.token_text(token)
    existing = self.find_local(name)
    if existing != nil && existing[2]
      self.emit_instruction2(Opcode::SET_CELL, existing[1], value)
      self.set_type_fact(existing[1], self.type_fact(value))
      return value
    end
    destination = if existing == nil
      self.define_local(name)
    else
      existing[1]
    end
    self.emit_instruction2(Opcode::MOVE, destination, value)
    self.set_type_fact(destination, self.type_fact(value))
    source_declaration = self.declared_type(value)
    if source_declaration != nil
      @declared_types.push([destination, source_declaration])
    end
    destination
  end

  def compile_ivar_write(token, value)
    if @current_class_index == nil && @current_module_index == nil
      self.fail("instance variable used outside a method")
      return 0
    end
    field_text = self.token_text(token)
    field_name = field_text.slice(1, field_text.length() - 1)
    if @current_module_index != nil && @current_class_index == nil
      @current_method_uses_state = true
      @builder.declare_module_field(@current_module_index, field_name)
      field_index = self.add_string(field_name)
      self.emit_instruction3(47, 0, field_index, value)
    else
      field_index = @builder.declare_field(@current_class_index, field_name)
      self.emit_instruction3(Opcode::SET_IVAR, 0, field_index, value)
    end
    value
  end

  def compile_ivar_read(token)
    if @current_class_index == nil && @current_module_index == nil
      self.fail("instance variable used outside a method")
      return 0
    end
    field_text = self.token_text(token)
    field_name = field_text.slice(1, field_text.length() - 1)
    destination = self.allocate_register()
    if @current_module_index != nil && @current_class_index == nil
      @current_method_uses_state = true
      @builder.declare_module_field(@current_module_index, field_name)
      field_index = self.add_string(field_name)
      self.emit_instruction3(46, destination, 0, field_index)
    else
      field_index = @builder.declare_field(@current_class_index, field_name)
      self.emit_instruction3(Opcode::GET_IVAR, destination, 0, field_index)
    end
    destination
  end

  # `receiver.method(args)` -- reached from parse_precedence's own
  # postfix-dot loop, so `receiver` can be any already-compiled
  # expression (a local, `self`, a call result, ...), not just an
  # identifier. Unlike a direct top-level call, the method name is
  # resolved by the VM at INVOKE time against the receiver's *runtime*
  # class, so there's no arity check here -- an ordinary ArityError
  # surfaces from the VM instead, mirroring compiler.c's own parse_invoke.
  def compile_invoke(receiver)
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected method name after '.'")
      return 0
    end
    name = self.token_text(@current)
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after method name")
      return 0
    end
    self.advance_token()
    method_name_index = self.add_string(name)
    parsed = self.parse_call_arguments()
    return 0 if parsed == nil
    destination = self.allocate_register()
    self.emit_byte(Opcode::INVOKE)
    self.emit_byte(destination)
    self.emit_byte(receiver)
    self.emit_byte(method_name_index)
    self.emit_byte(parsed[0])
    self.emit_byte(parsed[1])
    destination
  end

  def parse_index(receiver)
    self.advance_token()
    index = self.parse_expression()
    if @current.kind() != :right_bracket
      self.fail("expected ']' after index")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::INDEX_GET, destination, receiver, index)
    receiver_annotation = self.declared_type(receiver)
    element = nil
    if receiver_annotation != nil && receiver_annotation.length() == 1
      member = receiver_annotation[0]
      element = member[1] if self.resolve_type_name(member[0]) == Type::ARRAY
    end
    if element != nil
      @declared_types.push([destination, element])
      fact = self.annotation_single_type(element)
      self.set_type_fact(destination, fact) if fact != nil
    else
      value = nil
      if receiver_annotation != nil && receiver_annotation.length() == 1
        member = receiver_annotation[0]
        value = member[2] if self.resolve_type_name(member[0]) == Type::HASH
      end
      if value != nil
        @declared_types.push([destination, self.annotation_with_nil(value)])
      end
    end
    destination
  end

  def compile_break()
    kind = @current.kind()
    if @loops.length() == 0
      if kind == :break
        self.fail("'break' used outside a loop")
      elsif kind == :next
        self.fail("'next' used outside a loop")
      else
        self.fail("'redo' used outside a loop")
      end
      return self.allocate_register()
    end
    self.advance_token()
    has_value = @current.kind() != :newline && @current.kind() != :end && @current.kind() != :else && @current.kind() != :if && @current.kind() != :unless && @current.kind() != :eof
    frame = @loops[@loops.length() - 1]
    if kind != :break && has_value
      self.fail("next and redo do not accept values")
      return self.allocate_register()
    elsif has_value
      value = self.parse_expression()
      self.emit_instruction2(Opcode::MOVE, frame[0], value)
    end
    if kind == :break
      frame[1].push(self.emit_jump(Opcode::JUMP, 0))
    elsif kind == :next
      self.emit_byte(Opcode::JUMP)
      self.emit_byte(frame[2] / 256)
      self.emit_byte(mod(frame[2], 256))
    else
      self.emit_byte(Opcode::JUMP)
      self.emit_byte(frame[3] / 256)
      self.emit_byte(mod(frame[3], 256))
    end
    self.allocate_register()
  end

  # RETURN halts run_chunk unconditionally the instant it executes,
  # wherever it is in the bytecode. Postfix if/unless delimit a bare return;
  # compile_sequence's modifier layout makes the opcode itself conditional.
  def compile_return()
    if @current_function_index == -1
      self.fail("'return' used outside a function")
      return 0
    end
    self.advance_token()
    has_value = @current.kind() != :newline && @current.kind() != :end && @current.kind() != :else && @current.kind() != :if && @current.kind() != :unless && @current.kind() != :eof
    value = if has_value
      self.parse_expression()
    else
      self.allocate_register()
    end
    self.emit_type_check(value, @current_return_type) if @current_return_type != nil
    self.emit_instruction1(Opcode::RETURN, value)
    value
  end

  def compile_raise()
    self.advance_token()
    if @current.kind() == :newline || @current.kind() == :end || @current.kind() == :rescue || @current.kind() == :ensure || @current.kind() == :if || @current.kind() == :unless || @current.kind() == :eof
      if @current_exception == nil
        self.fail("bare 'raise' used outside rescue")
        return 0
      end
      self.emit_instruction1(Opcode::RAISE, @current_exception)
      return @current_exception
    end
    value = self.parse_expression()
    self.emit_instruction1(Opcode::RAISE, value)
    value
  end

  def compile_retry()
    self.advance_token()
    if @current_retry_target == nil
      self.fail("'retry' used outside rescue")
      return 0
    end
    self.emit_byte(Opcode::JUMP)
    self.emit_byte(@current_retry_target / 256)
    self.emit_byte(mod(@current_retry_target, 256))
    self.allocate_register()
  end

  def compile_rescue_clause(exception, retry_target, destination, seen_types)
    self.advance_token()
    rescue_local_count = @locals.length()
    if @current.kind() == :identifier
      @locals.push([self.token_text(@current), exception, false])
      self.advance_token()
    end
    types = self.parse_rescue_types(exception)
    type_index = 0
    while type_index < types.length() && !@failed
      seen_index = 0
      while seen_index < seen_types.length()
        if seen_types[seen_index] == types[type_index]
          self.fail("rescue type was already handled")
        elsif seen_types[seen_index] >= Type::CLASS_BASE && seen_types[seen_index] < 96 && types[type_index] >= Type::CLASS_BASE && types[type_index] < 96
          ancestor = seen_types[seen_index] - Type::CLASS_BASE
          child = types[type_index] - Type::CLASS_BASE
          while child != nil && child != ancestor
            entry = nil
            class_index = 0
            while class_index < @classes.length() && entry == nil
              entry = @classes[class_index] if @classes[class_index][1] == child
              class_index = class_index + 1
            end
            child = if entry == nil
              nil
            else
              entry[2]
            end
          end
          self.fail("rescue type is covered by an earlier clause") if child == ancestor
        end
        seen_index = seen_index + 1
      end
      seen_types.push(types[type_index]) unless @failed
      type_index = type_index + 1
    end
    return nil if @failed
    match_jumps = []
    type_index = 0
    while type_index < types.length()
      matched = self.allocate_register()
      self.emit_instruction3(Opcode::IS_TYPE, matched, exception, types[type_index])
      match_jumps.push(self.emit_jump(Opcode::JUMP_IF_TRUE, matched))
      type_index = type_index + 1
    end
    mismatch_jump = if types.length() > 0
      self.emit_jump(Opcode::JUMP, 0)
    else
      nil
    end
    return nil unless self.consume_block_start()
    type_index = 0
    while type_index < match_jumps.length()
      self.patch_jump(match_jumps[type_index], @code_count)
      type_index = type_index + 1
    end
    outer_exception = @current_exception
    outer_retry_target = @current_retry_target
    @current_exception = exception
    @current_retry_target = retry_target
    rescued = self.compile_sequence()
    @current_exception = outer_exception
    @current_retry_target = outer_retry_target
    rescued_fact = self.type_fact(rescued)
    rescued_declaration = self.declared_type(rescued)
    self.emit_instruction2(Opcode::MOVE, destination, rescued)
    finished = self.emit_jump(Opcode::JUMP, 0)
    while @locals.length() > rescue_local_count
      @locals.pop()
    end
    self.patch_jump(mismatch_jump, @code_count) if mismatch_jump != nil
    [finished, rescued_fact, rescued_declaration, mismatch_jump == nil]
  end

  def compile_begin()
    return 0 unless self.consume_block_start()
    original_facts = self.copy_type_facts()
    ensure_operand = @code_count + 1
    self.emit_byte(Opcode::PUSH_ENSURE)
    self.emit_byte(0)
    self.emit_byte(0)
    exception = self.allocate_register()
    retry_target = @code_count
    handler = self.emit_rescue_handler(exception)
    body = self.compile_sequence()
    body_fact = self.type_fact(body)
    body_declaration = self.declared_type(body)
    destination = self.allocate_register()
    self.emit_instruction2(Opcode::MOVE, destination, body)
    self.emit_byte(Opcode::POP_RESCUE)
    finished = self.emit_jump(Opcode::JUMP, 0)
    self.patch_jump(handler, @code_count)
    rescued_fact = body_fact
    rescued_declaration = body_declaration
    rescue_finished = []
    seen_rescue_types = []
    saw_rescue = false
    catch_all = false
    while @current.kind() == :rescue && !@failed
      if catch_all
        self.fail("rescue clause after catch-all is unreachable")
        break
      end
      saw_rescue = true
      clause = self.compile_rescue_clause(exception, retry_target, destination, seen_rescue_types)
      return destination if clause == nil
      rescue_finished.push(clause[0])
      rescued_fact = nil if rescued_fact != clause[1]
      rescued_declaration = nil if rescued_declaration != clause[2]
      catch_all = clause[3]
    end
    if saw_rescue && !catch_all
      self.emit_instruction1(Opcode::RAISE, exception)
    end
    if @current.kind() != :ensure && !saw_rescue
      self.fail("expected 'rescue' or 'ensure' after begin body")
      return destination
    end
    @builder.patch_byte(@current_function_index, handler - 9, 0) if saw_rescue
    self.patch_jump(finished, @code_count)
    if @current.kind() == :else
      self.advance_token()
      return destination unless self.consume_block_start()
      normal = self.compile_sequence()
      self.emit_instruction2(Opcode::MOVE, destination, normal)
      body_fact = self.type_fact(normal)
      body_declaration = self.declared_type(normal)
    end
    rescue_index = 0
    while rescue_index < rescue_finished.length()
      self.patch_jump(rescue_finished[rescue_index], @code_count)
      rescue_index = rescue_index + 1
    end
    self.emit_byte(Opcode::RUN_ENSURE)
    continuation_operand = @code_count
    self.emit_byte(0)
    self.emit_byte(0)
    self.patch_jump(ensure_operand, @code_count)
    if @current.kind() == :ensure
      self.advance_token()
      return destination unless self.consume_block_start()
      self.compile_sequence()
    end
    self.emit_byte(Opcode::END_ENSURE)
    if @current.kind() != :end
      self.fail("expected 'end' after begin body")
      return destination
    end
    self.advance_token()
    self.patch_jump(continuation_operand, @code_count)
    @type_facts = original_facts
    if body_fact != nil && body_fact == rescued_fact
      self.set_type_fact(destination, body_fact)
    end
    if body_declaration != nil && body_declaration == rescued_declaration
      @declared_types.push([destination, body_declaration])
    end
    destination
  end

  # --- control flow ---

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

  def apply_condition_fact(condition, narrowing, type_narrowing, when_true)
    if narrowing != nil && narrowing[0] == condition
      if when_true == narrowing[2]
        self.set_type_fact(narrowing[1], Type::NIL)
      else
        remaining = self.non_nil_single_type(self.declared_type(narrowing[1]))
        self.set_type_fact(narrowing[1], remaining) if remaining != nil
      end
    elsif type_narrowing != nil && type_narrowing[0] == condition
      if when_true
        self.set_type_fact(type_narrowing[1], type_narrowing[2])
      else
        remaining = self.remaining_single_type(
          self.declared_type(type_narrowing[1]), type_narrowing[2])
        self.set_type_fact(type_narrowing[1], remaining) if remaining != nil
      end
    end
  end

  def parse_if(inverted)
    condition = self.parse_expression()
    return 0 unless self.consume_block_start_or(:then)
    branch_condition = condition
    if inverted
      branch_condition = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, branch_condition, condition)
    end
    false_jump = self.emit_jump(Opcode::JUMP_IF_FALSE, branch_condition)
    destination = self.allocate_register()
    original_facts = self.copy_type_facts()
    narrowing = @pending_nil_narrowing
    type_narrowing = @pending_type_narrowing
    @pending_nil_narrowing = nil
    @pending_type_narrowing = nil
    self.apply_condition_fact(condition, narrowing, type_narrowing, !inverted)
    then_result = self.compile_sequence()
    then_fact = self.type_fact(then_result)
    then_declaration = self.declared_type(then_result)
    then_branch_facts = self.copy_type_facts()
    self.emit_instruction2(Opcode::MOVE, destination, then_result)
    end_jump = self.emit_jump(Opcode::JUMP, 0)
    self.patch_jump(false_jump, @code_count)

    end_consumed = false
    if @current.kind() == :else
      @type_facts = original_facts
      self.apply_condition_fact(condition, narrowing, type_narrowing, inverted)
      self.advance_token()
      self.skip_newlines() if @current.kind() == :newline
      else_result = self.compile_sequence()
      else_fact = self.type_fact(else_result)
      else_declaration = self.declared_type(else_result)
      else_branch_facts = self.copy_type_facts()
      self.emit_instruction2(Opcode::MOVE, destination, else_result)
    elsif @current.kind() == :elsif
      @type_facts = original_facts
      self.advance_token()
      else_result = self.parse_if(false)
      else_fact = self.type_fact(else_result)
      else_declaration = self.declared_type(else_result)
      else_branch_facts = self.copy_type_facts()
      self.emit_instruction2(Opcode::MOVE, destination, else_result)
      end_consumed = true
    else
      @type_facts = original_facts
      self.emit_instruction1(Opcode::NIL, destination)
      else_fact = Type::NIL
      else_declaration = nil
      else_branch_facts = original_facts
    end

    if !end_consumed && @current.kind() != :end
      self.fail("expected 'end' after if expression")
      return destination
    end
    self.advance_token() unless end_consumed
    self.patch_jump(end_jump, @code_count)
    self.merge_local_type_facts(original_facts, then_branch_facts,
                                else_branch_facts)
    if then_fact != nil && then_fact == else_fact
      self.set_type_fact(destination, then_fact)
    end
    if then_declaration != nil && then_declaration == else_declaration
      @declared_types.push([destination, then_declaration])
    end
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
    return 0 unless self.consume_block_start_or(:do)
    branch_condition = condition
    if inverted
      branch_condition = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, branch_condition, condition)
    end
    exit_jump = self.emit_jump(Opcode::JUMP_IF_FALSE, branch_condition)
    frame = [destination, [], loop_start, @code_count]
    entry_facts = self.copy_type_facts()
    @loops.push(frame)
    self.compile_sequence()
    @loops.pop()
    self.emit_byte(Opcode::JUMP)
    self.emit_byte(loop_start / 256)
    self.emit_byte(mod(loop_start, 256))
    self.patch_jump(exit_jump, @code_count)
    self.patch_breaks(frame)
    if @current.kind() != :end
      self.fail("expected 'end' after while expression")
      return 0
    end
    self.advance_token()
    @type_facts = entry_facts
    destination
  end

  def parse_loop()
    destination = self.allocate_register()
    self.emit_instruction1(Opcode::NIL, destination)
    return destination unless self.consume_block_start_or(:do)
    body_start = @code_count
    frame = [destination, [], body_start, body_start]
    @loops.push(frame)
    self.compile_sequence()
    @loops.pop()
    self.emit_byte(Opcode::JUMP)
    self.emit_byte(body_start / 256)
    self.emit_byte(mod(body_start, 256))
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
    return Precedence::EQUALITY if kind == :equal_equal || kind == :bang_equal || kind == :is
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
    while !@failed && (@current.kind() == :dot || @current.kind() == :left_bracket)
      left = if @current.kind() == :dot
        self.compile_invoke(left)
      else
        self.parse_index(left)
      end
    end
    while !@failed && self.token_precedence(@current.kind()) >= precedence
      operator = @current.kind()
      operator_precedence = self.token_precedence(operator)
      self.advance_token()
      if operator == :is
        if @current.kind() != :identifier
          self.fail("expected type after 'is'")
        else
          type_name = self.token_text(@current)
          type_id = self.resolve_type_name(type_name)
          if type_id >= 96 && type_id < 128
            self.fail("generic type variables cannot be used with 'is' before binding")
          end
          self.advance_token() unless @failed
          destination = self.allocate_register()
          self.emit_instruction3(Opcode::IS_TYPE, destination, left, type_id)
          if self.declared_type(left) != nil
            @pending_type_narrowing = [destination, left, type_id]
          end
          left = destination
        end
      elsif operator == :and_and || operator == :and || operator == :or_or || operator == :or
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
        self.apply_result_join(destination, left, right)
        left = destination
      elsif operator != :is
        right = self.parse_precedence(operator_precedence + 1)
        destination = self.allocate_register()
        self.emit_instruction3(self.binary_opcode(operator), destination, left, right)
        if operator == :equal_equal || operator == :bang_equal
          self.set_type_fact(destination, Type::BOOL)
          nil_when_true = operator == :equal_equal
          if self.type_fact(left) == Type::NIL && self.declared_type(right) != nil
            @pending_nil_narrowing = [destination, right, nil_when_true]
          elsif self.type_fact(right) == Type::NIL && self.declared_type(left) != nil
            @pending_nil_narrowing = [destination, left, nil_when_true]
          end
        elsif operator == :less || operator == :less_equal || operator == :greater || operator == :greater_equal
          self.set_type_fact(destination, Type::BOOL)
        else
          result_fact = self.binary_result_fact(operator, left, right)
          self.set_type_fact(destination, result_fact) if result_fact != nil
        end
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
    return self.parse_array() if kind == :left_bracket
    return self.parse_hash() if kind == :left_brace
    if kind == :self
      if @current_class_index == nil && @current_module_index == nil
        self.fail("'self' used outside a method")
        return 0
      end
      return 0
    end
    if kind == :instance_variable
      return self.compile_ivar_read(@previous)
    end
    if kind == :minus
      operand = self.parse_precedence(Precedence::PREFIX)
      destination = self.allocate_register()
      self.emit_instruction2(Opcode::NEGATE, destination, operand)
      operand_fact = self.type_fact(operand)
      if operand_fact == Type::INT || operand_fact == Type::FLOAT
        self.set_type_fact(destination, operand_fact)
      end
      return destination
    end
    if kind == :bang || kind == :not
      operand = self.parse_precedence(Precedence::PREFIX)
      destination = self.allocate_register()
      self.emit_instruction2(Opcode::NOT, destination, operand)
      self.set_type_fact(destination, Type::BOOL)
      return destination
    end
    return self.parse_if(false) if kind == :if
    return self.parse_if(true) if kind == :unless
    return self.parse_while(false) if kind == :while
    return self.parse_while(true) if kind == :until
    return self.parse_loop() if kind == :loop
    return self.compile_begin() if kind == :begin
    return self.parse_super() if kind == :super
    self.fail("expected expression")
    0
  end

  def parse_super()
    if @current_class_index == nil
      self.fail("'super' used outside a method")
      return 0
    end
    if @current_class_superclass_index == nil
      self.fail("'super' used in a class without a superclass")
      return 0
    end
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'super'")
      return 0
    end
    self.advance_token()
    parsed = self.parse_call_arguments()
    return 0 if parsed == nil
    destination = self.allocate_register()
    method_name_index = self.add_string(@current_method_name)
    self.emit_byte(Opcode::SUPER)
    self.emit_byte(destination)
    self.emit_byte(@current_class_index)
    self.emit_byte(method_name_index)
    self.emit_byte(parsed[0])
    self.emit_byte(parsed[1])
    destination
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

  def parse_array()
    elements = []
    self.skip_newlines()
    while !@failed && @current.kind() != :right_bracket
      if elements.length() == 32
        self.fail("array literal has too many elements")
      else
        elements.push(self.parse_expression())
        self.skip_newlines()
        if @current.kind() == :comma
          self.advance_token()
          self.skip_newlines()
        else
          break
        end
      end
    end
    if !@failed && @current.kind() != :right_bracket
      self.fail("expected ']' after array literal")
    else
      self.advance_token() unless @failed
    end
    base = self.allocate_register()
    index = 1
    while index < elements.length()
      self.allocate_register()
      index = index + 1
    end
    index = 0
    while index < elements.length()
      self.emit_instruction2(Opcode::MOVE, base + index, elements[index])
      index = index + 1
    end
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::ARRAY, destination, base, elements.length())
    self.set_type_fact(destination, Type::ARRAY)
    element_annotation = self.homogeneous_element_annotation(elements)
    if element_annotation != nil
      @declared_types.push([destination,
        [["Array", element_annotation, nil, -1, nil, nil]]])
    end
    destination
  end

  def parse_hash()
    keys = []
    values = []
    self.skip_newlines()
    while !@failed && @current.kind() != :right_brace
      if keys.length() == 16
        self.fail("hash literal has too many entries")
      else
        keys.push(self.parse_expression())
        if @current.kind() != :colon
          self.fail("expected ':' after hash key")
        else
          self.advance_token()
          values.push(self.parse_expression())
          self.skip_newlines()
          if @current.kind() == :comma
            self.advance_token()
            self.skip_newlines()
          else
            break
          end
        end
      end
    end
    return self.allocate_register() if @failed
    if !@failed && @current.kind() != :right_brace
      self.fail("expected '}' after hash literal")
    else
      self.advance_token() unless @failed
    end
    base = self.allocate_register()
    index = 1
    while index < keys.length() * 2
      self.allocate_register()
      index = index + 1
    end
    index = 0
    while index < keys.length()
      self.emit_instruction2(Opcode::MOVE, base + index * 2, keys[index])
      self.emit_instruction2(Opcode::MOVE, base + index * 2 + 1, values[index])
      index = index + 1
    end
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::HASH, destination, base, keys.length())
    self.set_type_fact(destination, Type::HASH)
    key_annotation = self.homogeneous_element_annotation(keys)
    value_annotation = self.homogeneous_element_annotation(values)
    if key_annotation != nil && value_annotation != nil
      @declared_types.push([destination,
        [["Hash", key_annotation, value_annotation, -1, nil, nil]]])
    end
    destination
  end

  # Dispatch precedence mirrors parse_name/parse_call in compiler.c: a
  # local variable holding a closure is called first (so a local named
  # `puts` shadows the print builtin, just like the real compiler's own
  # `callable_local>=0` check runs before any of parse_name's builtin
  # checks, all of which explicitly require `find_local(name)<0`),
  # builtins are checked before user functions, and a bare identifier
  # with no following `(` is always a local read.
  def parse_name()
    name = self.token_text(@previous)
    module_entry = nil
    index = 0
    while index < @modules.length()
      module_entry = @modules[index] if @modules[index][0] == name
      index = index + 1
    end
    if module_entry != nil && @current.kind() == :dot
      self.advance_token()
      return self.compile_module_singleton_call(module_entry)
    end
    if @current.kind() == :double_colon
      return self.parse_qualified_name(name, module_entry)
    end
    class_entry = self.find_class(name)
    if class_entry == nil && @current_module_name != nil
      class_entry = self.find_class(@current_module_name + "::" + name)
    end
    return self.compile_new_call(class_entry[1]) if class_entry != nil && @current.kind() == :dot
    local = self.find_local(name)
    return self.parse_file_open_call() if self.is_file_open_target(name, class_entry, local)
    dot_construct = self.dot_construct_id(name, class_entry, local)
    return self.parse_dot_construct_call(dot_construct) if dot_construct >= 0
    if local == nil && @current_module_index != nil
      constant_index = nil
      constants = @modules[@current_module_entry][2]
      index = 0
      while index < constants.length()
        constant_index = constants[index][1] if constants[index][0] == name
        index = index + 1
      end
      if constant_index == nil && @modules[@current_module_entry][8] != nil
        parent_name = @modules[@current_module_entry][8]
        while constant_index == nil && parent_name != nil
          index = 0
          while index < @modules.length()
            if @modules[index][0] == parent_name
              constants = @modules[index][2]
              parent_name = @modules[index][8]
            end
            index = index + 1
          end
          index = 0
          while index < constants.length()
            constant_index = constants[index][1] if constants[index][0] == name
            index = index + 1
          end
        end
      end
      if constant_index != nil
        destination = self.allocate_register()
        self.emit_instruction2(48, destination, constant_index)
        return destination
      end
    end
    return self.parse_name_call(name, local) if @current.kind() == :left_paren
    return self.compile_call(name) if @current.kind() == :left_bracket && local == nil
    if local == nil
      self.fail("undefined local variable")
      return 0
    end
    self.read_local(local)
  end

  def parse_name_call(name, local)
    return self.compile_closure_call(local) if local != nil
    return self.parse_print_call(true) if name == "puts"
    return self.parse_print_call(false) if name == "print"
    return self.parse_builtin_scalar_call(name) if self.is_builtin_scalar_target(name)
    return self.parse_math_unary_call(self.math_unary_id(name)) if self.is_math_unary_target(name)
    return self.parse_math_binary_call(4) if self.is_math_binary_target(name)
    self.compile_call(name)
  end

  def parse_qualified_name(name, module_entry)
    self.advance_token()
    if @current.kind() != :identifier
      self.fail("expected name after '::'")
      return 0
    end
    constant_name = self.token_text(@current)
    qualified_name = name + "::" + constant_name
    self.advance_token()
    nested_module = nil
    index = 0
    while index < @modules.length()
      nested_module = @modules[index] if @modules[index][0] == qualified_name
      index = index + 1
    end
    if nested_module != nil && @current.kind() == :dot
      self.advance_token()
      return self.compile_module_singleton_call(nested_module)
    end
    if nested_module != nil && @current.kind() == :double_colon
      self.advance_token()
      if @current.kind() != :identifier
        self.fail("expected name after '::'")
        return 0
      end
      constant_name = self.token_text(@current)
      self.advance_token()
      qualified_name = qualified_name + "::" + constant_name
      name = nil
      index = 0
      while index < @modules.length()
        name = @modules[index] if @modules[index][0] == qualified_name
        index = index + 1
      end
      if name != nil && @current.kind() == :dot
        self.advance_token()
        return self.compile_module_singleton_call(name)
      end
      if name == nil && @current.kind() == :dot
        name = self.find_class(qualified_name)
        return self.compile_new_call(name[1]) if name != nil
      end
      if name != nil && @current.kind() == :double_colon
        self.advance_token()
        if @current.kind() != :identifier
          self.fail("expected name after '::'")
          return 0
        end
        constant_name = self.token_text(@current)
        self.advance_token()
        module_entry = name
      else
        module_entry = nested_module
      end
    end
    if nested_module == nil && @current.kind() == :dot
      nested_module = self.find_class(qualified_name)
      return self.compile_new_call(nested_module[1]) if nested_module != nil
    end
    constant_index = nil
    if module_entry != nil
      index = 0
      while index < module_entry[2].length()
        constant_index = module_entry[2][index][1] if module_entry[2][index][0] == constant_name
        index = index + 1
      end
    end
    if constant_index == nil
      self.fail("undefined namespaced class")
      return 0
    end
    destination = self.allocate_register()
    self.emit_instruction2(48, destination, constant_index)
    destination
  end

  def compile_module_singleton_call(module_entry)
    if @current.kind() != :identifier
      self.fail("undefined module singleton function")
      return 0
    end
    name = self.token_text(@current)
    self.advance_token()
    if @current.kind() == :equal
      name = name + "="
      self.advance_token()
    end
    descriptor = nil
    index = 0
    while index < module_entry[7].length()
      descriptor = module_entry[7][index] if module_entry[7][index][0] == name
      index = index + 1
    end
    if descriptor == nil
      self.fail("undefined module singleton function")
      return 0
    end
    if @current.kind() != :left_paren
      self.fail("expected '(' after singleton function")
      return 0
    end
    self.advance_token()
    parsed = self.parse_call_arguments()
    return 0 if parsed == nil
    if parsed[1] != descriptor[2]
      self.fail("wrong number of arguments")
      return 0
    end
    base = self.allocate_register()
    index = 0
    while index < parsed[1]
      self.allocate_register()
      self.emit_instruction2(Opcode::MOVE, base + index + 1, parsed[0] + index)
      index = index + 1
    end
    destination = self.allocate_register()
    self.emit_byte(Opcode::CALL)
    self.emit_byte(destination)
    self.emit_byte(descriptor[1])
    self.emit_byte(base)
    self.emit_byte(parsed[1] + 1)
    destination
  end

  def compile_new_call(class_index)
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "new"
      self.fail("expected 'new' after class name")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'new'")
      return 0
    end
    self.advance_token()
    parsed = self.parse_call_arguments()
    return 0 if parsed == nil
    destination = self.allocate_register()
    self.emit_byte(Opcode::NEW)
    self.emit_byte(destination)
    self.emit_byte(class_index)
    self.emit_byte(parsed[0])
    self.emit_byte(parsed[1])
    self.set_type_fact(destination, Type::CLASS_BASE + class_index)
    destination
  end

  def is_builtin_scalar_target(name)
    return false if self.find_function(name) != nil
    return true if name == "gets"
    return true if name == "chr"
    return true if name == "to_f"
    return true if name == "to_i"
    name == "to_sym"
  end

  def parse_builtin_scalar_call(name)
    return self.parse_gets_call() if name == "gets"
    return self.parse_chr_call() if name == "chr"
    return self.parse_to_float_call() if name == "to_f"
    return self.parse_to_int_call() if name == "to_i"
    self.parse_to_sym_call()
  end

  def parse_gets_call()
    self.advance_token()
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction1(Opcode::GETS, destination)
    destination
  end

  def parse_scalar_conversion_call(opcode, type_fact)
    self.advance_token()
    self.skip_newlines()
    source = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction2(opcode, destination, source)
    self.set_type_fact(destination, type_fact)
    destination
  end

  def parse_chr_call()
    self.parse_scalar_conversion_call(Opcode::CHR, Type::STRING)
  end

  def parse_to_float_call()
    self.parse_scalar_conversion_call(Opcode::TO_FLOAT, Type::FLOAT)
  end

  def parse_to_int_call()
    self.parse_scalar_conversion_call(Opcode::TO_INT, Type::INT)
  end

  def parse_to_sym_call()
    self.parse_scalar_conversion_call(Opcode::TO_SYMBOL, Type::SYMBOL)
  end

  def is_math_unary_target(name)
    return false if self.find_function(name) != nil
    return true if name == "sqrt"
    return true if name == "sin"
    return true if name == "cos"
    name == "tan"
  end

  def is_math_binary_target(name)
    return false if self.find_function(name) != nil
    name == "pow"
  end

  def math_unary_id(name)
    return 0 if name == "sqrt"
    return 1 if name == "sin"
    return 2 if name == "cos"
    3
  end

  def parse_math_unary_call(id)
    self.advance_token()
    self.skip_newlines()
    source = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::MATH_UNARY, destination, source, id)
    self.set_type_fact(destination, Type::FLOAT)
    destination
  end

  def parse_math_binary_call(id)
    self.advance_token()
    self.skip_newlines()
    left = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :comma
      self.fail("expected ',' between arguments")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    right = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_byte(Opcode::MATH_BINARY)
    self.emit_byte(destination)
    self.emit_byte(left)
    self.emit_byte(right)
    self.emit_byte(id)
    self.set_type_fact(destination, Type::FLOAT)
    destination
  end

  def dot_construct_id(name, class_entry, local)
    return -1 if class_entry != nil
    return -1 if local != nil
    return -1 if self.find_function(name) != nil
    return -1 if @current.kind() != :dot
    return 0 if name == "Fiber"
    return 1 if name == "Regexp"
    return 2 if name == "TCPSocket"
    return 3 if name == "TCPServer"
    -1
  end

  def parse_dot_construct_call(id)
    return self.parse_fiber_new_call() if id == 0
    return self.parse_regexp_new_call() if id == 1
    return self.parse_tcp_connect_call() if id == 2
    self.parse_tcp_listen_call()
  end

  def parse_fiber_new_call()
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "new"
      self.fail("expected 'new' after 'Fiber'")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'Fiber.new'")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    callable = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after Fiber.new argument")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction2(Opcode::FIBER_NEW, destination, callable)
    destination
  end

  def parse_regexp_new_call()
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "new"
      self.fail("expected 'new' after 'Regexp'")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'Regexp.new'")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    pattern = self.parse_expression()
    self.skip_newlines()
    options = 0
    if @current.kind() == :comma
      self.advance_token()
      self.skip_newlines()
      options = self.parse_expression()
      self.skip_newlines()
    else
      options = self.allocate_register()
      zero = self.add_constant(0)
      self.emit_instruction2(Opcode::CONSTANT, options, zero)
    end
    if @current.kind() != :right_paren
      self.fail("expected ')' after Regexp.new arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::REGEXP_NEW, destination, pattern, options)
    destination
  end

  def parse_tcp_connect_call()
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "connect"
      self.fail("expected 'connect' after 'TCPSocket'")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'TCPSocket.connect'")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    host = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :comma
      self.fail("expected ',' after TCPSocket.connect host")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    port = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after TCPSocket.connect arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::TCP_CONNECT, destination, host, port)
    destination
  end

  def parse_tcp_listen_call()
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "listen"
      self.fail("expected 'listen' after 'TCPServer'")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'TCPServer.listen'")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    port = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after TCPServer.listen argument")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction2(Opcode::TCP_LISTEN, destination, port)
    destination
  end

  def is_file_open_target(name, class_entry, local)
    return false if class_entry != nil
    return false if local != nil
    return false if self.find_function(name) != nil
    return false if @current.kind() != :dot
    name == "File"
  end

  def parse_file_open_call()
    self.advance_token()
    if @current.kind() != :identifier || self.token_text(@current) != "open"
      self.fail("expected 'open' after 'File'")
      return 0
    end
    self.advance_token()
    if @current.kind() != :left_paren
      self.fail("expected '(' after 'File.open'")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    path_register = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :comma
      self.fail("expected ',' after File.open path")
      return 0
    end
    self.advance_token()
    self.skip_newlines()
    mode_register = self.parse_expression()
    self.skip_newlines()
    if @current.kind() != :right_paren
      self.fail("expected ')' after File.open arguments")
      return 0
    end
    self.advance_token()
    destination = self.allocate_register()
    self.emit_instruction3(Opcode::FILE_OPEN, destination, path_register, mode_register)
    destination
  end

  def parse_literal()
    destination = self.allocate_register()
    if @previous.kind() == :nil
      self.set_type_fact(destination, Type::NIL)
      # Sole-writer register; run_chunk's zero-init already covers nil,
      # matching compiler.c's parse_literal (no NIL opcode emitted here).
    else
      value = if @previous.kind() == :true
        1
      else
        0
      end
      self.emit_instruction2(Opcode::BOOL, destination, value)
      self.set_type_fact(destination, Type::BOOL)
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
    self.set_type_fact(destination, Type::INT)
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
    self.set_type_fact(destination, Type::FLOAT)
    destination
  end

  def decode_string_range(start, length)
    raw = @source.slice(start, length)
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
    span = @previous
    finish = span.start() + span.length() - 1
    piece = span.start() + 1
    result = nil
    while piece < finish && !@failed
      index = piece
      while index + 1 < finish
        if @source[index] == "\\"
          index = index + 2
          next
        end
        break if @source[index] == "#" && @source[index + 1] == "{"
        index = index + 1
      end
      index = finish if index + 1 >= finish
      literal_register = self.allocate_register()
      literal = self.add_string(self.decode_string_range(piece, index - piece))
      self.emit_instruction2(Opcode::STRING, literal_register, literal)
      if result == nil
        result = literal_register
      else
        joined = self.allocate_register()
        self.emit_instruction3(Opcode::ADD, joined, result, literal_register)
        result = joined
      end
      break if index >= finish

      outer_lexer = @lexer
      outer_current = @current
      outer_previous = @previous
      embedded = Lexer.new(@source)
      embedded.restore_state(index + 2, index + 2, span.line(),
        span.column() + index - span.start() + 2, span.line(),
        span.column() + index - span.start() + 2)
      @lexer = embedded
      @current = @lexer.next_token()
      value = self.parse_expression()
      if @current.kind() != :right_brace
        self.fail("expected '}' after interpolation")
      end
      close = @current.start()
      @lexer = outer_lexer
      @current = outer_current
      @previous = outer_previous
      converted = self.allocate_register()
      self.emit_instruction2(Opcode::TO_STRING, converted, value)
      joined = self.allocate_register()
      self.emit_instruction3(Opcode::ADD, joined, result, converted)
      result = joined
      piece = close + 1
    end
    if result == nil
      result = self.allocate_register()
      literal = self.add_string("")
      self.emit_instruction2(Opcode::STRING, result, literal)
    end
    self.set_type_fact(result, Type::STRING)
    result
  end
end

# Convenience entry point mirroring diamond_compile's own two-step shape
# (build, then execute via diamond_program_chunk + diamond_vm_run) --
# here, ProgramBuilder#run does both at once. Returns the parser on
# failure (so the caller can read .error_message()) or runs the program
# and returns its result on success.
def parse_and_run(path)
  source = File.open(path, "r").read()
  builder = ProgramBuilder.new()
  expanded = builder.expand_source(path, source)
  parser = Parser.new(expanded, builder)
  if parser.compile()
    builder.run()
  else
    raise RuntimeError.new(parser.error_message())
  end
end
