# Self-hosting Phase 2: a Diamond-language port of src/lexer.c's
# diamond_lexer_next. Token kinds are Symbols (:integer, :left_paren, ...)
# named after the C DIAMOND_TOKEN_* constants in lower_snake_case with the
# prefix dropped -- there is no enum keyword, and Symbols already give
# readable, content-compared, GC-managed values with no interning table
# needed (see docs/roadmap.md's Symbol entry).
#
# Diamond has no char type, so every character comparison here works on
# Int codepoints (String#ord()) rather than the C original's `char`
# values -- code_at(index) is the one new primitive the C file didn't
# need, standing in for C's "index past the end reads a safe '\0'"
# implicit-null-terminator behavior with an explicit bounds check instead.
#
# See tests/lexer_diff.sh for the differential harness comparing this
# lexer's token stream against the C one across every tests/cases/*.di
# file.

class Token
  attr_reader kind, start, length, line, column

  def initialize(kind, start, length, line, column)
    @kind = kind
    @start = start
    @length = length
    @line = line
    @column = column
  end
end

class Lexer
  def initialize(source)
    @source = source
    @start = 0
    @current = 0
    @line = 1
    @column = 1
    @token_line = 1
    @token_column = 1
  end

  def next_token()
    while !self.at_end?()
      code = self.code_at(@current)
      if code == " ".ord() || code == "\t".ord() || code == "\r".ord()
        self.advance()
        next
      elsif code == "#".ord()
        reset = false
        if @current + 7 <= @source.length()
          if @source.slice(@current, 7) == "#line 1"
            reset = true
          end
        end
        if reset
          after_index = @current + 7
          after_ok = after_index >= @source.length()
          if !after_ok
            after_code = self.code_at(after_index)
            after_ok = after_code == "\n".ord() || after_code == 0
          end
          if after_ok
            @line = 0
          end
        end
        while !self.at_end?() && self.code_at(@current) != "\n".ord()
          self.advance()
        end
        next
      else
        break
      end
    end

    @start = @current
    @token_line = @line
    @token_column = @column

    if self.at_end?()
      return self.make_token(:eof)
    end

    code = self.advance()

    if code >= "0".ord() && code <= "9".ord()
      return self.scan_number()
    end
    if code == "\"".ord()
      return self.scan_string()
    end
    if self.identifier_start?(code)
      while self.identifier_part?(self.code_at(@current))
        self.advance()
      end
      if self.code_at(@current) == "?".ord() || self.code_at(@current) == "!".ord()
        self.advance()
      end
      return self.make_token(self.identifier_kind())
    end
    if code == "@".ord() && self.code_at(@current) == "@".ord()
      if self.identifier_start?(self.code_at(@current + 1))
        self.advance()
        while self.identifier_part?(self.code_at(@current))
          self.advance()
        end
        return self.make_token(:class_variable)
      end
    end
    if code == "@".ord() && self.identifier_start?(self.code_at(@current))
      while self.identifier_part?(self.code_at(@current))
        self.advance()
      end
      return self.make_token(:instance_variable)
    end

    self.scan_punctuation(code)
  end

  # Snapshot-and-restore, not a struct copy: Diamond has no direct
  # instance-field access from outside a class other than through methods,
  # so this is how a caller peeks ahead by one token (build a copy, advance
  # only the copy, discard it) without a lexer clone constructor overload --
  # Diamond has no overloading, and adding an alternate-state constructor
  # parameter list to `initialize` risked the existing, already-verified
  # single-argument construction path. Used by Phase 3's Parser for
  # single-token lookahead (e.g. "is the next token '=' ", deciding
  # whether an identifier starts an assignment), mirroring compiler.c's own
  # `DiamondLexer lookahead = compiler->lexer;` snapshot idiom.
  def clone()
    copy = Lexer.new(@source)
    copy.restore_state(@start, @current, @line, @column, @token_line, @token_column)
    copy
  end

  def restore_state(start, current, line, column, token_line, token_column)
    @start = start
    @current = current
    @line = line
    @column = column
    @token_line = token_line
    @token_column = token_column
  end

  private

  # Split out of next_token() itself, not just for readability: register
  # allocation is monotonic per function body and never recycled (see
  # Compiler.next_register in src/compiler.c), so one function covering
  # every branch of next_token() -- whitespace/comment skipping, number/
  # string/identifier/instance-variable dispatch, *and* the full
  # punctuation chain -- exhausted the 256-register budget outright
  # (confirmed by trying it first: "program needs too many registers").
  # Splitting into smaller methods isn't optional restructuring here, it's
  # required to fit in a single function's register file at all -- worth
  # remembering for Phase 3's much larger parser/emitter port.
  def scan_punctuation(code)
    return self.make_token(:left_paren) if code == "(".ord()
    return self.make_token(:right_paren) if code == ")".ord()
    return self.make_token(:left_bracket) if code == "[".ord()
    return self.make_token(:right_bracket) if code == "]".ord()
    return self.make_token(:left_brace) if code == "{".ord()
    return self.make_token(:right_brace) if code == "}".ord()
    return self.make_token(:comma) if code == ",".ord()
    return self.make_token(:question) if code == "?".ord()
    if code == ".".ord()
      if self.match?(".".ord())
        return self.make_token(:dot_dot_dot) if self.match?(".".ord())
        return self.make_token(:dot_dot)
      end
      return self.make_token(:dot)
    end
    if code == ":".ord()
      return self.scan_colon()
    end
    if code == "|".ord()
      if self.match?("|".ord())
        return self.make_token(:or_or_equal) if self.match?("=".ord())
        return self.make_token(:or_or)
      end
      return self.make_token(:pipe)
    end
    if code == "&".ord()
      if self.match?("&".ord())
        return self.make_token(:and_and_equal) if self.match?("=".ord())
        return self.make_token(:and_and)
      end
      return self.make_token(:ampersand)
    end
    if code == "+".ord()
      return self.make_token(:plus_equal) if self.match?("=".ord())
      return self.make_token(:plus)
    end
    if code == "-".ord()
      return self.make_token(:minus_equal) if self.match?("=".ord())
      return self.make_token(:arrow) if self.match?(">".ord())
      return self.make_token(:minus)
    end
    if code == "*".ord()
      return self.make_token(:star_equal) if self.match?("=".ord())
      return self.make_token(:star)
    end
    if code == "/".ord()
      return self.make_token(:slash_equal) if self.match?("=".ord())
      return self.make_token(:slash)
    end
    if code == "%".ord()
      return self.make_token(:percent_equal) if self.match?("=".ord())
      return self.make_token(:percent)
    end
    return self.make_token(:caret) if code == "^".ord()
    return self.make_token(:newline) if code == "\n".ord()
    return self.make_token(:newline) if code == ";".ord()
    if code == "=".ord()
      return self.make_token(:equal_equal) if self.match?("=".ord())
      return self.make_token(:equal)
    end
    if code == "!".ord()
      return self.make_token(:bang_equal) if self.match?("=".ord())
      return self.make_token(:bang)
    end
    if code == "<".ord()
      if self.match?("=".ord())
        return self.make_token(:spaceship) if self.match?(">".ord())
        return self.make_token(:less_equal)
      end
      return self.make_token(:less_less) if self.match?("<".ord())
      return self.make_token(:less)
    end
    if code == ">".ord()
      return self.make_token(:greater_equal) if self.match?("=".ord())
      return self.make_token(:greater_greater) if self.match?(">".ord())
      return self.make_token(:greater)
    end
    self.make_token(:error)
  end

  def code_at(index)
    if index >= @source.length()
      0
    else
      @source[index].ord()
    end
  end

  def at_end?()
    @current >= @source.length()
  end

  def advance()
    code = self.code_at(@current)
    @current = @current + 1
    if code == "\n".ord()
      @line = @line + 1
      @column = 1
    else
      @column = @column + 1
    end
    code
  end

  def match?(expected_code)
    return false if self.code_at(@current) != expected_code
    self.advance()
    true
  end

  def identifier_start?(code)
    (code >= "a".ord() && code <= "z".ord()) || (code >= "A".ord() && code <= "Z".ord()) || code == "_".ord()
  end

  def identifier_part?(code)
    self.identifier_start?(code) || (code >= "0".ord() && code <= "9".ord())
  end

  def text_equals?(text)
    return false if @current - @start != text.length()
    @source.slice(@start, @current - @start) == text
  end

  def identifier_kind()
    return :if if self.text_equals?("if")
    return :unless if self.text_equals?("unless")
    return :then if self.text_equals?("then")
    return :else if self.text_equals?("else")
    return :elsif if self.text_equals?("elsif")
    return :case if self.text_equals?("case")
    return :when if self.text_equals?("when")
    return :end if self.text_equals?("end")
    return :while if self.text_equals?("while")
    return :until if self.text_equals?("until")
    return :do if self.text_equals?("do")
    return :loop if self.text_equals?("loop")
    return :true if self.text_equals?("true")
    return :false if self.text_equals?("false")
    return :nil if self.text_equals?("nil")
    return :not if self.text_equals?("not")
    return :and if self.text_equals?("and")
    return :or if self.text_equals?("or")
    return :def if self.text_equals?("def")
    return :closure if self.text_equals?("closure")
    return :class if self.text_equals?("class")
    return :interface if self.text_equals?("interface")
    return :module if self.text_equals?("module")
    return :include if self.text_equals?("include")
    return :private if self.text_equals?("private")
    return :protected if self.text_equals?("protected")
    return :public if self.text_equals?("public")
    return :attr_reader if self.text_equals?("attr_reader")
    return :attr_writer if self.text_equals?("attr_writer")
    return :attr_accessor if self.text_equals?("attr_accessor")
    return :attr_predicate if self.text_equals?("attr_predicate")
    return :attr if self.text_equals?("attr")
    return :module_function if self.text_equals?("module_function")
    return :alias_method if self.text_equals?("alias_method")
    return :delegate if self.text_equals?("delegate")
    return :self if self.text_equals?("self")
    return :super if self.text_equals?("super")
    return :return if self.text_equals?("return")
    return :break if self.text_equals?("break")
    return :next if self.text_equals?("next")
    return :redo if self.text_equals?("redo")
    return :raise if self.text_equals?("raise")
    return :retry if self.text_equals?("retry")
    return :yield if self.text_equals?("yield")
    return :begin if self.text_equals?("begin")
    return :rescue if self.text_equals?("rescue")
    return :ensure if self.text_equals?("ensure")
    return :is if self.text_equals?("is")
    :identifier
  end

  def make_token(kind)
    Token.new(kind, @start, @current - @start, @token_line, @token_column)
  end

  # Scans zero or more additional digits (and digit-separator underscores)
  # starting at @current. Returns false the moment an underscore isn't
  # immediately followed by another digit -- an invalid separator, which
  # (mirroring lexer.c exactly) still consumes that character before
  # signalling the error, so the caller's ERROR token span covers it.
  def scan_digits()
    loop do
      next_code = self.code_at(@current)
      if next_code >= "0".ord() && next_code <= "9".ord()
        self.advance()
      elsif next_code == "_".ord()
        after_code = self.code_at(@current + 1)
        if after_code < "0".ord() || after_code > "9".ord()
          self.advance()
          return false
        end
        self.advance()
      else
        break
      end
    end
    true
  end

  def scan_number()
    return self.make_token(:error) unless self.scan_digits()
    is_float = false
    if self.code_at(@current) == ".".ord() && self.code_at(@current + 1) >= "0".ord() && self.code_at(@current + 1) <= "9".ord()
      self.advance()
      return self.make_token(:error) unless self.scan_digits()
      is_float = true
    end
    if self.code_at(@current) == "e".ord() || self.code_at(@current) == "E".ord()
      peek = @current + 1
      peek = peek + 1 if self.code_at(peek) == "+".ord() || self.code_at(peek) == "-".ord()
      if self.code_at(peek) >= "0".ord() && self.code_at(peek) <= "9".ord()
        self.advance()
        self.advance() if self.code_at(@current) == "+".ord() || self.code_at(@current) == "-".ord()
        return self.make_token(:error) unless self.scan_digits()
        is_float = true
      end
    end
    return self.make_token(:float) if is_float
    self.make_token(:integer)
  end

  def scan_string()
    loop do
      return self.make_token(:error) if self.at_end?()
      break if self.code_at(@current) == "\"".ord()
      if self.code_at(@current) == "#".ord() && self.code_at(@current + 1) == "{".ord()
        return self.make_token(:error) unless self.scan_interpolation()
      else
        if self.code_at(@current) == "\\".ord() && self.code_at(@current + 1) != 0
          self.advance()
        end
        self.advance()
      end
    end
    self.advance()
    self.make_token(:string)
  end

  def scan_interpolation()
    self.advance()
    self.advance()
    depth = 1
    while !self.at_end?() && depth > 0
      embedded = self.advance()
      if embedded == "\"".ord()
        while !self.at_end?() && self.code_at(@current) != "\"".ord()
          if self.code_at(@current) == "\\".ord() && self.code_at(@current + 1) != 0
            self.advance()
          end
          self.advance()
        end
        self.advance() unless self.at_end?()
      elsif embedded == "{".ord()
        depth = depth + 1
      elsif embedded == "}".ord()
        depth = depth - 1
      end
    end
    depth == 0
  end

  def scan_colon()
    return self.make_token(:double_colon) if self.match?(":".ord())
    glued = false
    if @start > 0
      prev_code = self.code_at(@start - 1)
      glued = self.identifier_part?(prev_code) || prev_code == ")".ord() || prev_code == "]".ord() || prev_code == "}".ord() || prev_code == "\"".ord()
    end
    if !glued && self.identifier_start?(self.code_at(@current))
      while self.identifier_part?(self.code_at(@current))
        self.advance()
      end
      if self.code_at(@current) == "?".ord() || self.code_at(@current) == "!".ord()
        self.advance()
      end
      return self.make_token(:symbol)
    end
    self.make_token(:colon)
  end
end
