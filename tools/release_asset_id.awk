# Extract the URL id of the object with the exact requested asset name.
# Track JSON objects rather than splitting on punctuation inside strings or
# assuming a particular layout. Only string-valued name/url properties matter.
{ json = json $0 "\n" }
END {
    for (i = 1; i <= length(json); i++) {
        c = substr(json, i, 1)
        if (c ~ /[[:space:]]/) continue
        if (c == "{") {
            depth++
            names[depth] = urls[depth] = keys[depth] = ""
        } else if (c == "}") {
            if (names[depth] == ENVIRON["ASSET_NAME"] &&
                urls[depth] ~ /^https:\/\/api.github.com\/repos\/[^/]+\/[^/]+\/releases\/assets\/[0-9]+$/) {
                sub(/^.*\//, "", urls[depth])
                print urls[depth]
                exit
            }
            depth--
        } else if (c == ":") {
            keys[depth] = text
        } else if (c == "\"") {
            text = ""
            for (i++; i <= length(json); i++) {
                c = substr(json, i, 1)
                if (c == "\"") break
                if (c == "\\") {
                    c = substr(json, ++i, 1)
                    if (c == "n") c = "\n"
                    else if (c == "r") c = "\r"
                    else if (c == "t") c = "\t"
                    else if (c == "b") c = "\b"
                    else if (c == "f") c = "\f"
                    # Retain unicode escapes literally rather than guessing;
                    # escapes in unrelated properties must not abort lookup.
                    else if (c != "\\" && c != "\"" && c != "/") c = "\\" c
                }
                text = text c
            }
            if (previous == ":") {
                if (keys[depth] == "name") names[depth] = text
                else if (keys[depth] == "url") urls[depth] = text
            }
            c = "string"
        }
        previous = c
    }
}
