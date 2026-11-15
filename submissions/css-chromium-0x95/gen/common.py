#!/usr/bin/env python3
"""Shared helpers for the gen/*.py modules (Python 3 stdlib only).

A tiny expression DSL whose trees can be evaluated in Python (fast differential tests against reference
implementations, no Chrome needed) and rendered to CSS (`calc()` / `if(style(...))`). Logical property names are bound
to the real custom-property names at render / eval time, so one tree describes the logic of every byte position.

    s, c = Pr('s'), Pr('c')
    e = If([(s.eq(1) & c.ge(48), s + 1)], 0)
    render(e, {'s': '--s3', 'c': '--c3'})  ->  if(style(--s3: 1) and style(--c3 >= 48): calc(var(--s3) + 1); else: 0)
    evaluate(e, {'s': 1, 'c': 50})         ->  2

Only integer math is used (floor division and floored modulo behave identically in Python and CSS for the values used).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def constants():
    with open(os.path.join(HERE, 'constants.json')) as f:
        return json.load(f)


# ---------------------------------------------------------------------------------------------------------------------
# expressions


class X:
    def __add__(a, b): return Op('+', a, lift(b))
    def __radd__(a, b): return Op('+', lift(b), a)
    def __sub__(a, b): return Op('-', a, lift(b))
    def __rsub__(a, b): return Op('-', lift(b), a)
    def __mul__(a, b): return Op('*', a, lift(b))
    def __rmul__(a, b): return Op('*', lift(b), a)
    def __floordiv__(a, b): return Div(a, b)
    def __mod__(a, b): return Mod(a, b)
    def __neg__(a): return Op('-', K(0), a)


class K(X):
    def __init__(self, n): self.n = int(n)


class Pr(X):
    """Reference to a (logical) custom property; also the source of conditions."""
    def __init__(self, name): self.name = name
    def eq(self, k): return Cmp(self.name, '==', k)
    def ne(self, k): return Cmp(self.name, '!=', k)
    def lt(self, k): return Cmp(self.name, '<', k)
    def le(self, k): return Cmp(self.name, '<=', k)
    def gt(self, k): return Cmp(self.name, '>', k)
    def ge(self, k): return Cmp(self.name, '>=', k)
    def rng(self, lo, hi): return And(Cmp(self.name, '>=', lo), Cmp(self.name, '<=', hi))
    def isin(self, *ks): return Or(*[Cmp(self.name, '==', k) for k in ks])


class Op(X):
    def __init__(self, op, a, b): self.op, self.a, self.b = op, a, b


class Div(X):          # floor(a / k)
    def __init__(self, a, k): self.a, self.k = lift(a), int(k)


class Mod(X):          # mod(a, k)
    def __init__(self, a, k): self.a, self.k = lift(a), int(k)


class Min(X):
    def __init__(self, *xs): self.xs = [lift(x) for x in xs]


class Max(X):
    def __init__(self, *xs): self.xs = [lift(x) for x in xs]


class Pow(X):          # pow(base, e)
    def __init__(self, base, e): self.base, self.e = lift(base), lift(e)


class If(X):
    """cases: [(Cond, X), ...] tried in order; default: X (the `else:` branch, always present)."""
    def __init__(self, cases, default): self.cases, self.default = [(c, lift(v)) for c, v in cases], lift(default)


class Inh(X):
    """The CSS-wide keyword `inherit`, only valid as the `else:` branch of a top-level if(): hold the parent's value of
    custom property `name` without reading it (so no cycle); python evaluation reads env['inh:' + name]."""
    def __init__(self, name): self.name = name


def lift(x):
    return x if isinstance(x, X) else K(x)


class Cond:
    def __and__(a, b): return And(a, b)
    def __or__(a, b): return Or(a, b)
    def __invert__(a): return Not(a)


class Cmp(Cond):
    def __init__(self, name, op, k): self.name, self.op, self.k = name, op, int(k)


class And(Cond):
    def __init__(self, *cs):
        self.cs = []
        for c in cs:
            self.cs.extend(c.cs if isinstance(c, And) else [c])


class Or(Cond):
    def __init__(self, *cs):
        self.cs = []
        for c in cs:
            self.cs.extend(c.cs if isinstance(c, Or) else [c])


class Not(Cond):
    def __init__(self, c): self.c = c


# ---------------------------------------------------------------------------------------------------------------------
# python evaluation

def evaluate(x, env):
    if isinstance(x, K): return x.n
    if isinstance(x, Pr): return env[x.name]
    if isinstance(x, Inh): return env['inh:' + x.name]
    if isinstance(x, Op):
        a, b = evaluate(x.a, env), evaluate(x.b, env)
        return a + b if x.op == '+' else a - b if x.op == '-' else a * b
    if isinstance(x, Div): return evaluate(x.a, env) // x.k
    if isinstance(x, Mod): return evaluate(x.a, env) % x.k
    if isinstance(x, Min): return min(evaluate(v, env) for v in x.xs)
    if isinstance(x, Max): return max(evaluate(v, env) for v in x.xs)
    if isinstance(x, Pow): return evaluate(x.base, env) ** evaluate(x.e, env)
    if isinstance(x, If):
        for c, v in x.cases:
            if test(c, env): return evaluate(v, env)
        return evaluate(x.default, env)
    raise TypeError(x)


_OPS = {'==': lambda a, b: a == b, '!=': lambda a, b: a != b, '<': lambda a, b: a < b, '<=': lambda a, b: a <= b,
        '>': lambda a, b: a > b, '>=': lambda a, b: a >= b}


def test(c, env):
    if isinstance(c, Cmp): return _OPS[c.op](env[c.name], c.k)
    if isinstance(c, And): return all(test(v, env) for v in c.cs)
    if isinstance(c, Or): return any(test(v, env) for v in c.cs)
    if isinstance(c, Not): return not test(c.c, env)
    raise TypeError(c)


# ---------------------------------------------------------------------------------------------------------------------
# CSS rendering

def _num(n):
    return str(n) if n >= 0 else '(%d)' % n


def inner(x, b, ctx=0):
    """Expression text valid inside calc(). ctx: 0 top / argument, 1 operand of + or -, 2 operand of *"""
    if isinstance(x, K): return _num(x.n)
    if isinstance(x, Pr): return 'var(%s)' % b[x.name]
    if isinstance(x, Op):
        if x.op == '*':
            t = '%s*%s' % (inner(x.a, b, 2), inner(x.b, b, 2))
            return '(%s)' % t if ctx == 3 else t
        t = '%s %s %s' % (inner(x.a, b, 1), x.op, inner(x.b, b, 3 if x.op == '-' else 1))
        return '(%s)' % t if ctx else t
    if isinstance(x, Div): return 'round(down,%s/%d)' % (inner(x.a, b, 2), x.k)
    if isinstance(x, Mod): return 'mod(%s,%d)' % (inner(x.a, b), x.k)
    if isinstance(x, Min): return 'min(%s)' % ','.join(inner(v, b) for v in x.xs)
    if isinstance(x, Max): return 'max(%s)' % ','.join(inner(v, b) for v in x.xs)
    if isinstance(x, Pow): return 'pow(%s,%s)' % (inner(x.base, b), inner(x.e, b))
    if isinstance(x, If): return render(x, b)
    raise TypeError(x)


def render(x, b):
    """A complete CSS value for a declaration."""
    if isinstance(x, K): return str(x.n)
    if isinstance(x, Inh): return 'inherit'
    if isinstance(x, If):
        parts = ['%s:%s' % (cond(c, b), render(v, b)) for c, v in x.cases]
        parts.append('else:%s' % render(x.default, b))
        return 'if(%s)' % ';'.join(parts)
    if isinstance(x, Pr): return 'var(%s)' % b[x.name]
    return 'calc(%s)' % inner(x, b)


def _isnot(c):
    return isinstance(c, Not) or (isinstance(c, Cmp) and c.op == '!=')


def cond(c, b):
    # grammar: `not X` cannot be an operand of and/or without parentheses; and/or cannot be mixed unparenthesized
    if isinstance(c, Cmp):
        n = b[c.name]
        if c.op == '==': return 'style(%s:%d)' % (n, c.k)
        if c.op == '!=': return 'not style(%s:%d)' % (n, c.k)
        return 'style(%s %s %d)' % (n, c.op, c.k)
    if isinstance(c, And):
        return ' and '.join('(%s)' % cond(v, b) if isinstance(v, Or) or _isnot(v) else cond(v, b) for v in c.cs)
    if isinstance(c, Or):
        return ' or '.join('(%s)' % cond(v, b) if isinstance(v, And) or _isnot(v) else cond(v, b) for v in c.cs)
    if isinstance(c, Not):
        v = c.c
        return 'not %s' % (cond(v, b) if isinstance(v, Cmp) and v.op != '!=' else '(%s)' % cond(v, b))
    raise TypeError(c)


def simplify_names(c):
    return c


# ---------------------------------------------------------------------------------------------------------------------
# declarations (one per property per position) + registry

class Decl:
    """`lhs: expr` with logical->real name binding. `lhs` is the real property name (with leading --)."""
    __slots__ = ('lhs', 'expr', 'bind')

    def __init__(self, lhs, expr, bind):
        self.lhs, self.expr, self.bind = lhs, expr, bind

    def css(self):
        return '%s:%s;' % (self.lhs, render(self.expr, self.bind))


class _View:
    __slots__ = ('env', 'bind')

    def __init__(self, env, bind): self.env, self.bind = env, bind

    def __getitem__(self, k): return self.env[self.bind[k]]


def run_decls(decls, env):
    """Evaluate declarations in order. env: real name -> int (modified in place)."""
    for d in decls:
        env[d.lhs] = evaluate(d.expr, _View(env, d.bind))
    return env


class Registry:
    """Property registrations of one module: name -> (syntax, inherits, initial)."""

    def __init__(self):
        self.p = {}

    def add(self, name, inherits=False, initial=0, syntax='<integer>'):
        if not name.startswith('--'):
            name = '--' + name
        self.p[name] = (syntax, bool(inherits), initial)

    def listing(self):
        return [(n, s, i, v) for n, (s, i, v) in sorted(self.p.items())]


def css_props(registry_items):
    return ''.join('@property %s{syntax:"%s";inherits:%s;initial-value:%s}\n'
                   % (n, s, 'true' if i else 'false', v) for n, s, i, v in registry_items)
