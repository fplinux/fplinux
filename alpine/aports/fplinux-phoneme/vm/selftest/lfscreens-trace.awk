# SPDX-License-Identifier: GPL-2.0-only

# Number screens by first appearance in the compared trace. Subsequent
# references must use the same number, even if two screens share a title.
function screen_id(id) {
    if (!(id in screen_ids))
        screen_ids[id] = ++screen_count
    return "#" screen_ids[id]
}

/^\[lf\] stack: / {
    line = "[lf] stack: "
    rest = substr($0, length(line) + 1)
    while (match(rest, /^#[0-9]+ (app|list|alert) "[^"]*"($| \| )/)) {
        entry = substr(rest, 1, RLENGTH)
        rest = substr(rest, RLENGTH + 1)
        id_end = index(entry, " ")
        line = line screen_id(substr(entry, 1, id_end - 1)) substr(entry, id_end)
    }
    print line rest
    next
}

/^\[lf\] (rows|keys) |^\[lf\] alert #[0-9]+ [a-z]+ text=/ {
    prefix = "[lf] " $2 " "
    rest = substr($0, length(prefix) + 1)
    if (match(rest, /^#[0-9]+[ :]/))
        rest = screen_id(substr(rest, 1, RLENGTH - 1)) substr(rest, RLENGTH)
    print prefix rest
    next
}

/^(PM-LFSCREENS |\[lf\] (stack|rows|keys|script))/ { print }
