# Byte-level tokenization: every possible byte value (0-255) is its
# own token id, vocab_size is always exactly 256. Chosen over word- or
# BPE-level tokenization deliberately -- it needs no vocabulary-
# building pass over the corpus first (works on *any* input file
# unchanged, including non-ASCII/UTF-8 text, since Diamond's own String
# is a raw byte buffer, not UTF-8-validated -- see url.di's own note in
# skindicate.dia for the same property used the same way there), and
# it's simple enough to implement correctly in a few lines. The
# tradeoff: sequences are byte-long, not word-long, so the same amount
# of *text* needs a longer seq_len / more training steps than a
# word/subword tokenizer would need -- acceptable for this scaffold,
# not something a production-scale model would actually want.
class ByteTokenizer
  def self.vocab_size() = 256

  def self.encode(text)
    ids = []
    i = 0
    while i < text.length()
      ids.push(text[i].ord())
      i += 1
    end
    ids
  end

  def self.decode(ids)
    text = ""
    i = 0
    while i < ids.length()
      text = text + ids[i].chr()
      i += 1
    end
    text
  end
end
