emoji = JSON.parse("\"\\uD83D\\uDE00\"")
round_tripped = JSON.parse(JSON.stringify(emoji)) == emoji

unpaired_high_rejected = false
begin
  JSON.parse("\"\\uD83D\"")
rescue error: JSONError
  unpaired_high_rejected = true
end

unpaired_low_rejected = false
begin
  JSON.parse("\"\\uDE00\"")
rescue error: JSONError
  unpaired_low_rejected = true
end

[emoji, emoji.length(), round_tripped, unpaired_high_rejected, unpaired_low_rejected]
