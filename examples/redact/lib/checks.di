# Extra checks a pattern match must pass before it counts. A regex can say
# "13 to 19 digits" or "four dotted numbers", but not "passes the card
# checksum" or "every octet is at most 255".

# The Luhn checksum every payment card number carries in its last digit.
def luhn?(text: String) -> Bool
  # Keep only the digits (the text may contain spaces or dashes), and reverse
  # them so the check digit is first. Real cards are 13 to 19 digits.
  digits = text.gsub(Regexp.new("[^0-9]"), "").chars().reverse()
  return false if digits.length() < 13 || digits.length() > 19

  # Luhn: starting from the digit NEXT to the check digit, double every
  # second digit (a doubled value over 9 has 9 subtracted, which is the same
  # as adding its two digits), add everything up, and the total must be a
  # multiple of 10. This catches nearly all single-digit typos, so random
  # 16-digit numbers (order numbers, IDs) are mostly NOT mistaken for cards.
  sum = 0

  digits.each_with_index() do |digit, index|
    value = digit.to_i()
    if index % 2 == 1
      value *= 2
      value -= 9 if value > 9
    end
    sum += value
  end
  sum % 10 == 0
end

# A dotted quad is an address only if each part is 0-255 (the regexp already
# guarantees four parts of 1-3 digits, so "999.1.1.1" would otherwise match).
def ipv4?(text: String) -> Bool
  text.split(".").all?() do |octet| octet.to_i() <= 255 end
end
