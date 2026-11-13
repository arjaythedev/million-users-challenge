#!/usr/bin/env python3
"""HMAC-SHA256 chain on top of gen/sha256.py (Python 3 stdlib only).

Element layout:

  #root (once, at init)      holds the key bytes `--k0..--k63` (zero padded key, <= 64 bytes) and computes the
                             two midstates:  inner  --sb<k>l/h  = compress(IV, key ^ 0x36..)
                                             outer  --ko<k>l/h  = compress(IV, key ^ 0x5c..)
  per request, nested:
      .S0 / .S1  one inner message block each (64 bytes `--c0..--c63` on the element, already padded by the
                 caller with 0x80 + 64-bit length of (64 + message length) * 8). S0 reads --sb* and writes --sa*,
                 S1 reads --sa* and writes --sb* (parity alternates, so there is no self-reference).
      .O0 / .O1  outer block, no inputs: O0 hashes the digest left by an S0 (--sa*), O1 the one left by an S1
                 (--sb*); both read the outer midstate --ko* and write the HMAC to --so<k>l/h.

`rules()` returns the CSS (without @property; call sha256.property_rules() afterwards).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sha256 as S


def rules(root='#root', key_prefix='--k', wrap=None, gate=None, **opts):
    """All rules for the chain above. `gate` = optional at-rule prelude wrapping the per-request rules only
    (e.g. '@container style(--need-auth: 1)'); the root midstate rules are never gated."""
    out = []
    kb = ['var(%s%d)' % (key_prefix, i) for i in range(64)]
    for pad, pre, tag in ((0x36, 'sb', 'ki'), (0x5c, 'ko', 'ko')):
        msg = S.words_from_byte_exprs([S.byte_xor_const(b, pad) for b in kb])
        out.append(S.compress_rule(root, None, pre, msg=msg, tag=tag, **opts))
    out.append(S.compress_rule('.S0', 'sb', 'sa', tag='h', wrap=gate, **opts))
    out.append(S.compress_rule('.S1', 'sa', 'sb', tag='h', wrap=gate, **opts))
    out.append(S.compress_rule('.O0', 'ko', 'so', msg=S.digest_block_msg('sa', 96), tag='h', wrap=gate, **opts))
    out.append(S.compress_rule('.O1', 'ko', 'so', msg=S.digest_block_msg('sb', 96), tag='h', wrap=gate, **opts))
    return ''.join(out)


if __name__ == '__main__':
    sys.stdout.write(rules())
    sys.stdout.write(S.property_rules())
