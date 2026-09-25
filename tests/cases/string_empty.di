def blank?(s: String) -> Bool = s.strip().empty?()
["".empty?(), "x".empty?(), blank?("  "), blank?(" a ")]
