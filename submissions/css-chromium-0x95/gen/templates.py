#!/usr/bin/env python3
"""Response templates as CSS strings: the page (web/glue.js) fills them in, it does not know the JSON shape or the HTTP head.

Each template is a registered, NON-inherited `<string>` custom property whose `initial-value` is the template (so nothing is
inherited by the request elements; the glue reads them once from #root at init). Markers are `%` + one letter and are filled
in a single pass over the template (data is never re-scanned). Which template a response uses is the CSS output `kind`
(V: 7 when the CSS answers alone, D: 1..8); the status is the CSS output `st`.

    --tpl-1  health ok          %u uptime_s
    --tpl-2  health degraded    %e database error text (JSON-escaped by the glue)
    --tpl-3  feed               %r the rows, each filled into --tpl-row, joined with --tpl-sep
    --tpl-4  one post           %r the row
    --tpl-5  created post       %r the row (body = the trimmed request body, author = the username JSON literal of the token)
    --tpl-6  liked              %p post id
    --tpl-7  error              %m message (--msg-N from build.py, N = the CSS output `err`)
    --tpl-8  already liked      %p post id
    --tpl-row                   %i id  %b body (escaped)  %d created_at  %a author (a complete JSON string literal)  %l like_count
    --tpl-ts                    created_at: every X is replaced by the next digit of YYYYMMDDhhmmssmmm (the CSS outputs dt / tm)
    --tpl-sep                   row separator
    --tpl-head                  status line + headers: %s status  %w reason phrase  %c --tpl-close if the CSS output `cl` is 1  %n Content-Length
    --reason-<status>           reason phrases
"""
REASONS = {200: 'OK', 201: 'Created', 400: 'Bad Request', 401: 'Unauthorized', 404: 'Not Found', 500: 'Internal Server Error',
           503: 'Service Unavailable'}

TEMPLATES = [
    ('--tpl-1', '{"status":"ok","db":"ok","uptime_s":%u}'),
    ('--tpl-2', '{"status":"degraded","db":"unreachable","error":"%e"}'),
    ('--tpl-3', '{"posts":[%r]}'),
    ('--tpl-4', '{"post":%r}'),
    ('--tpl-5', '{"post":%r}'),
    ('--tpl-6', '{"liked":true,"already_liked":false,"post_id":%p}'),
    ('--tpl-7', '{"error":"%m"}'),
    ('--tpl-8', '{"liked":true,"already_liked":true,"post_id":%p}'),
    ('--tpl-row', '{"id":%i,"body":"%b","created_at":"%d","author":%a,"like_count":%l}'),
    ('--tpl-ts', 'XXXX-XX-XXTXX:XX:XX.XXXZ'),
    ('--tpl-sep', ','),
    ('--tpl-head', 'HTTP/1.1 %s %w\r\nContent-Type: application/json\r\n%cContent-Length: %n\r\n\r\n'),
    ('--tpl-close', 'Connection: close\r\n'),
] + [('--reason-%d' % k, v) for k, v in sorted(REASONS.items())]


def css_string(s):
    """a CSS string token: double quotes, `"` and `\\` escaped, control characters as hex escapes (the space ends the escape)"""
    out = ['"']
    for ch in s:
        if ch in '"\\':
            out.append('\\' + ch)
        elif ord(ch) < 32:
            out.append('\\%x ' % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def rules():
    return ''


def properties():
    return [(n, '<string>', False, css_string(v)) for n, v in TEMPLATES]
