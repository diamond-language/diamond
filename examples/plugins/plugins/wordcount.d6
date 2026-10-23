# wordcount: a well-behaved plugin. Counts words and vowels in ARGV[0]
# and prints a one-line report. Touches nothing outside the process, so it
# runs the same whether the host trusts it or not.

def main(argv)
  if argv.empty?()
    puts("wordcount: expected text as argv[0]")
    return 64
  end
  text = argv[0]

  # Words: split on spaces, ignoring empty pieces from repeated spaces.
  words = text.split(" ").reject() do |w| w.empty?() end

  # Vowels: count by scanning every character.
  vowels = 0
  index = 0

  while index < text.length()
    if "aeiouAEIOU".include?(text[index])
      vowels = vowels + 1
    end
    index = index + 1
  end

  # Longest word: keep the better of the best so far and each word (the
  # first of equal length wins).
  longest = words.reduce("") do |best, w| w.length() > best.length() ? w : best end
  puts("words=#{words.length()} vowels=#{vowels} longest=#{longest}")
  0
end

exit(main(ARGV))
