def trimmed(text: String) -> String = "[#{text.lstrip()}|#{text.rstrip()}]"
[trimmed("  a b  "), trimmed("\tx\n"), trimmed("")]
