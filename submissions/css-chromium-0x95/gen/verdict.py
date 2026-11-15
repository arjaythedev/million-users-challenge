#!/usr/bin/env python3
"""V stage: the verdict. Precedence exactly like the Go handlers (examples/go/handlers.go):

    unknown route -> 404 not found
    create/like:  au 0 -> 401 missing bearer token, au 1 -> 401 invalid or expired token, au 2 -> 401 invalid token payload
    get/like:     id invalid -> 400 invalid post id
    create:       bj 0 -> 400 malformed JSON body; bj 1 or trimmed body empty -> 400 body is required;
                  trimmed length > 500 UTF-16 units -> 400 body must be at most 500 characters
    otherwise     act = which query the front runs (1 SELECT 1, 2 feed, 3 post by id, 4 insert post, 5 insert like).

The outputs are the `counter-reset` names (exact int32) of the V element:
    act st err kind cl pidh pidl uidh uidl uns une bep blen pb0..pb7 ub0..ub7
(bep = epoch of the winning `body` key, blen = trimmed UTF-16 length: the glue needs them to pick/cut the body text;
 kind = 7 (error response) when the CSS answers by itself (act 0), else 0; cl = the connection closes after the response;
 pb0..pb7 / ub0..ub7 = the 8 big-endian bytes of the post id / user id, i.e. the Bind parameters of the int8 columns, computed
 only for the acts that bind them: post id for act 3 and 5, user id for act 4 and 5 (the other bytes are 0).)
"""
from common import *

REG = Registry()
REG.add('--vc')

# inherited inputs from the other stages (fallback registrations, see build.py weak_properties)
_IN = [('--rt', 0), ('--idok', 0), ('--pidh', 0), ('--pidl', 0), ('--au', 0), ('--uidh', 0), ('--uidl', 0), ('--un-s', 0),
       ('--un-e', 0), ('--bj', 1), ('--blen', 0), ('--bep', 0)]

rt, idok, au, bj, blen, vc = (Pr(n) for n in ('--rt', '--idok', '--au', '--bj', '--blen', '--vc'))

# verdict case -> (act, status, err)
CASES = [
    (0, 404, 0),   # 0 unknown route
    (0, 401, 3),   # 1 missing bearer
    (0, 401, 4),   # 2 invalid or expired token
    (0, 401, 5),   # 3 invalid payload
    (0, 400, 1),   # 4 invalid post id
    (0, 400, 6),   # 5 malformed JSON
    (0, 400, 7),   # 6 body required
    (0, 400, 8),   # 7 body too long
    (1, 200, 0),   # 8 health
    (2, 200, 0),   # 9 feed
    (3, 200, 0),   # 10 get post
    (4, 201, 0),   # 11 create
    (5, 201, 0),   # 12 like
]


def case_expr():
    authrt = rt.eq(4) | rt.eq(5)
    return If([
        (rt.eq(0), 0),
        (rt.eq(1), 8),
        (rt.eq(2), 9),
        (authrt & au.eq(0), 1),
        (authrt & au.eq(1), 2),
        (authrt & au.eq(2), 3),
        ((rt.eq(3) | rt.eq(5)) & idok.eq(0), 4),
        (rt.eq(3), 10),
        (rt.eq(5), 12),
        (bj.eq(0), 5),
        (bj.eq(1) | blen.eq(0), 6),
        (blen.gt(500), 7),
    ], 11)


# outputs derived from the case index
act_e = If([(vc.ge(8), vc - 7)], 0)
kind_e = If([(vc.le(7), 7)], 0)
st_e = If([(vc.ge(11), 201), (vc.ge(8), 200), (vc.ge(4), 400), (vc.ge(1), 401)], 404)
err_e = If([(vc.ge(8), 0), (vc.ge(5), vc + 1), (vc.eq(4), 1), (vc.ge(1), vc + 2)], 0)


class _Id(dict):
    def __missing__(self, k): return k


# base-1e6 limbs -> 8 big-endian bytes of the int8 bind parameter (one calc() each: exact doubles; ids >= 2^53 are approximate)
PID = Pr('--pidh') * 1000000 + Pr('--pidl')
UID = Pr('--uidh') * 1000000 + Pr('--uidl')


def be_byte(v, k):
    """byte k (0 = most significant) of the 64-bit big-endian value v"""
    return Mod(v, 256) if k == 7 else Mod(Div(v, 256 ** (7 - k)), 256)


def rules():
    b = _Id()
    outs = [('act', act_e), ('st', st_e), ('err', err_e), ('kind', kind_e), ('cl', Pr('--cl') + 0)]
    outs += [(n, Pr('--' + n) + 0) for n in ('pidh', 'pidl', 'uidh', 'uidl')]
    outs += [('uns', Pr('--un-s') + 0), ('une', Pr('--un-e') + 0), ('bep', Pr('--bep') + 0), ('blen', Pr('--blen') + 0)]
    outs += [('pb%d' % k, If([(vc.isin(10, 12), be_byte(PID, k))], 0)) for k in range(8)]
    outs += [('ub%d' % k, If([(vc.isin(11, 12), be_byte(UID, k))], 0)) for k in range(8)]
    cr = ' '.join('%s %s' % (n, render(e, b)) for n, e in outs)
    return '.V{--vc:%s;counter-reset:%s}\n' % (render(case_expr(), b), cr)


def properties():
    return REG.listing()


def weak_properties():
    return [(n, '<integer>', True, i) for n, i in _IN] + [('--cl', '<integer>', True, 0)]


# ------------------------------------------------------------------------------------------------ python reference
def decide(env):
    """env: dict of the input props (names without leading --, '-' kept) -> (act, status, err)"""
    e = {'--' + k: v for k, v in env.items()}
    e['--vc'] = evaluate(case_expr(), _Id2(e))
    return CASES[e['--vc']]


class _Id2:
    def __init__(self, e): self.e = e
    def __getitem__(self, k): return self.e[k]
