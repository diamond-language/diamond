module Dials

  # Request query-string/form-body parsing -- protocol-generic, no
  # knowledge of routes or controllers. Dials::Router.dispatch calls
  # Params.parse itself and merges the result with captured path segments;
  # call it directly too if a handler wants query/body params without
  # going through the router at all.
  class Params
    # -1 for anything that isn't a hex digit -- Params.decode's own signal
    # to treat a "%" that isn't actually followed by two hex digits as a
    # literal character instead of raising. Compares `.ord()` values
    # rather than the characters themselves (String does support
    # ordering) -- an ASCII byte-range check reads more directly this
    # way than a lexicographic String comparison would.
    def self.hex_digit_value(ch: String) -> Int
      code = ch.ord()
      if code >= "0".ord() && code <= "9".ord()
        code - "0".ord()
      elsif code >= "a".ord() && code <= "f".ord()
        code - "a".ord() + 10
      elsif code >= "A".ord() && code <= "F".ord()
        code - "A".ord() + 10
      else
        -1
      end
    end

    # application/x-www-form-urlencoded decoding: "+" is a space, and
    # "%XX" is the byte whose value the two hex digits spell out (e.g. a
    # comma, not in the unreserved character set, submits as "%2C") --
    # both halves are required, not just the "+" one, or a field
    # containing a reserved character (a comma, a space encoded as "%20"
    # instead of "+", ...) comes back through #parse with the raw
    # percent-encoding still in it instead of the real character.
    def self.decode(value: String) -> String
      sb = StringBuilder.new()
      i = 0
      length = value.length()
      while i < length
        ch = value[i]
        if ch == "+"
          sb.append(" ")
          i += 1
        elsif ch == "%" && i + 2 < length
          high = Params.hex_digit_value(value[i + 1])
          low = Params.hex_digit_value(value[i + 2])
          if high >= 0 && low >= 0
            sb.append(chr(high * 16 + low))
            i += 3
          else
            sb.append(ch)
            i += 1
          end
        else
          sb.append(ch)
          i += 1
        end
      end
      sb.to_s()
    end

    def self.parse(request)
      values = {}
      source = request["body"]
      if request["method"] == "GET"
        source = request["path"]
        question = source.index_of("?")
        source = if question == nil then "" else source.slice(question + 1, source.length()) end
      end
      source.split("&").each() do |pair|
        equals = pair.index_of("=")
        if equals != nil
          values[Params.decode(pair.slice(0, equals))] = Params.decode(pair.slice(equals + 1, pair.length()))
        end
      end
      values
    end
  end

end
