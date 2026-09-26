# Extra checks a pattern match must pass before it counts. A regex can say
# "13 to 19 digits" or "four dotted numbers", but not "passes the card
# checksum" or "every octet is at most 255".

# The Luhn checksum every payment card number carries in its last digit.
def luhn?(text: String) -> Bool
  digits = text.gsub(Regexp.new("[^0-9]"), "").chars().reverse()
  return false if digits.length() < 13 || digits.length() > 19
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

def ipv4?(text: String) -> Bool
  text.split(".").all?() do |octet| octet.to_i() <= 255 end
end
