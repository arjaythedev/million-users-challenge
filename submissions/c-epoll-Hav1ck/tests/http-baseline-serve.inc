// Frozen serve() from baseline 275983052ceed6e6de9f610cb6bb545c107475db.
// Only serve/tchar identifiers renamed for independent differential testing.
static long baseline_serve(th_t *t, int fd, char *buf, size_t n) {
  conn_t *c = &conns[fd]; size_t used = 0;
  while (used < n && c->open && !c->busy && !c->closing && !c->out) {
    char *p = buf + used, *e = buf + n;
    char *he = memmem(p, e - p, "\r\n\r\n", 4);
    if (!he) { if ((size_t)(e - p) >= MAXREQ) return -1; break; }
    if ((size_t)(he + 4 - p) > MAXREQ) return -1;
    char *le = memmem(p, he + 2 - p, "\r\n", 2);
    if (!le) return -1;
    char *m = p, *sp1 = memchr(m, ' ', le - m); if (!sp1 || sp1 == m) return -1;
    for (char *d = m; d < sp1; d++) if (!oracle_tchar((unsigned char)*d)) return -1;
    char *path = sp1 + 1, *sp2 = memchr(path, ' ', le - path); if (!sp2 || sp2 == path) return -1;
    for (char *d = path; d < sp2; d++) if ((unsigned char)*d <= 32 || (unsigned char)*d == 127) return -1;
    int http10 = le - sp2 - 1 == 8 && !memcmp(sp2 + 1, "HTTP/1.0", 8);
    if (!http10 && !(le - sp2 - 1 == 8 && !memcmp(sp2 + 1, "HTTP/1.1", 8))) return -1;
    const char *authh = NULL; size_t al = 0, clen = 0;
    int close = http10, expect = 0, seen_length = 0, must_close = 0, keepalive = 0; unsigned enc = 0;
    for (char *h = le + 2; h < he + 2;) {
      char *hl = memmem(h, he + 2 - h, "\r\n", 2); if (!hl) return -1;
      char *col = memchr(h, ':', hl - h); if (!col || col == h) return -1;
      for (char *d = h; d < col; d++) if (!oracle_tchar((unsigned char)*d)) return -1;
      for (char *d = col + 1; d < hl; d++) if (((unsigned char)*d < 32 && *d != '\t') || (unsigned char)*d == 127) return -1;
      const char *v = col + 1, *ve = hl; ows(&v, &ve);
      size_t kn = col - h;
      if (hdr_is(h, kn, "content-length", 14)) {
        if (v == ve) return -1;
        size_t len = 0;
        for (const char *d = v; d < ve; d++) {
          if (*d < '0' || *d > '9' || len > (MAXREQ - (size_t)(*d - '0')) / 10) return -1;
          len = len * 10 + *d - '0';
        }
        if (seen_length && len != clen) return -1;
        seen_length = 1; clen = len;
      } else if (hdr_is(h, kn, "authorization", 13)) {
        if (authh) return -1;
        authh = v; al = ve - v;
      } else if (hdr_is(h, kn, "connection", 10)) {
        must_close |= has_token(v, ve, "close", 5); keepalive |= has_token(v, ve, "keep-alive", 10);
      } else if (hdr_is(h, kn, "accept-encoding", 15)) enc |= encoding_flags(v, ve);
      else if (hdr_is(h, kn, "expect", 6)) expect |= hdr_is(v, ve - v, "100-continue", 12);
      else if (hdr_is(h, kn, "transfer-encoding", 17)) return -1;
      h = hl + 2;
    }
    close = must_close || (http10 && !keepalive); want_gz = encoding_gzip(enc);
    if (clen > MAXREQ - (size_t)(he + 4 - p)) return -1;
    char *body = he + 4;
    if ((size_t)(e - body) < clen) {
      if (expect && !c->continued && !c->eof) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        c->continued = 1; send_output(t, fd, cont, sizeof cont - 1, NULL, 0, 0);
      }
      break;
    }
    c->continued = 0;
    used = body + clen - buf;
    handle(t, fd, m, sp1 - m, path, sp2 - path, authh, al, body, clen, close);
    if (!c->busy) send_resp(t, fd, close);
  }
  return used;
}
