# PROOF.bend: the proofs of LAWS.bend, one def Laws.<name> per law.
# Agent-written. The gate is scripts/verify.sh: bend PROOF.bend and
# bend PROOF.bend --verdict must both print ALL PROOFS CHECK.
#
# Method. classify is a decision tree. Bend unfolds it only after its
# scrutinees are known, and a match must follow binder order (the route,
# then the id inside it, then the auth, then the body), so every proof
# repeats the matches classify makes, in that order, until classify
# computes. Then either {==} closes the goal, or a hypothesis has become a
# false equation between True{} and False{} and the case is refuted.
import Base
import ../src/request.bend as Req
import ../src/respond.bend as Rsp
import ../src/policy.bend as Pol
import ./LAWS.bend as Laws

# Refuting a false Bool equation
# ------------------------------
# Disc(b) is Empty when b is True{} and Unit when b is False{}. Rewriting
# e : {False{} == True{}} through Disc(_) turns the goal Disc(True{}), which
# is Empty, into Disc(False{}), which is Unit, so Unit{} proves it. Then
# Empty.absurd gives any goal G. G is spelled out at each use: Bend has no
# hole that infers it.

def Disc(b: Bool) -> Data:
  match b:
    case True{}:
      Empty
    case False{}:
      Unit

def false_ne_true(-G: Type, e: {False{} == True{} : Bool}) -> G:
  Empty.absurd(G, %e : Disc(_)
    Unit{})

def true_ne_false(-G: Type, e: {True{} == False{} : Bool}) -> G:
  false_ne_true(G, Equal.sym(Bool, True{}, False{}, e))

# auth_gate
# ---------
# Four routes are open, so p : {False{} == True{}} refutes them. On the two
# protected routes, the three non-User verdicts compute to 401 and the User
# verdict contradicts u : {True{} == False{}}. LikePost matches its id
# first because the id sits in the route, which comes before the auth.

def Goal.auth_gate(route: Req.Route, auth: Req.Auth, body: Req.Body) -> Type:
  {Req.status(Req.classify(Req.Request{route, auth, body})) == 401 : U32}

def Laws.auth_gate(route, auth, body, p, u):
  match route:
    case Req.Health{}:
      false_ne_true(Goal.auth_gate(Req.Health{}, auth, body), p)
    case Req.Feed{}:
      false_ne_true(Goal.auth_gate(Req.Feed{}, auth, body), p)
    case Req.PostById{pid}:
      false_ne_true(Goal.auth_gate(Req.PostById{pid}, auth, body), p)
    case Req.CreatePost{}:
      match auth:
        case Req.User{uid, name}:
          true_ne_false(Goal.auth_gate(Req.CreatePost{}, Req.User{uid, name}, body), u)
        case Req.BadPayload{}:
          {==}
        case Req.BadToken{}:
          {==}
        case Req.NoBearer{}:
          {==}
    case Req.LikePost{pid}:
      match pid:
        case Req.ValidId{id}:
          match auth:
            case Req.User{uid, name}:
              true_ne_false(Goal.auth_gate(Req.LikePost{Req.ValidId{id}}, Req.User{uid, name}, body), u)
            case Req.BadPayload{}:
              {==}
            case Req.BadToken{}:
              {==}
            case Req.NoBearer{}:
              {==}
        case Req.InvalidId{}:
          match auth:
            case Req.User{uid, name}:
              true_ne_false(Goal.auth_gate(Req.LikePost{Req.InvalidId{}}, Req.User{uid, name}, body), u)
            case Req.BadPayload{}:
              {==}
            case Req.BadToken{}:
              {==}
            case Req.NoBearer{}:
              {==}
    case Req.Unknown{}:
      false_ne_true(Goal.auth_gate(Req.Unknown{}, auth, body), p)

# auth_before_id and invalid_id_no_query
# --------------------------------------
# Both name their route and id outright, so classify computes: no match.

def Laws.auth_before_id(body):
  {==}

def Laws.invalid_id_no_query(auth, body):
  {==}

# bad_body_no_write
# -----------------
# CreatePost with no User rejects, so is_write is False{}. With a User, the
# four non-text bodies reject. Only Trimmed{s} runs a write, and there
# t : {True{} == False{}} is false.

def Goal.bad_body_no_write(auth: Req.Auth, body: Req.Body) -> Type:
  {Req.is_write(Req.classify(Req.Request{Req.CreatePost{}, auth, body})) == False{} : Bool}

def Laws.bad_body_no_write(auth, body, t):
  match auth:
    case Req.User{uid, name}:
      match body:
        case Req.Trimmed{s}:
          true_ne_false(Goal.bad_body_no_write(Req.User{uid, name}, Req.Trimmed{s}), t)
        case Req.TooLong{}:
          {==}
        case Req.Missing{}:
          {==}
        case Req.NoneSent{}:
          {==}
        case Req.NotJson{}:
          {==}
    case Req.BadPayload{}:
      {==}
    case Req.BadToken{}:
      {==}
    case Req.NoBearer{}:
      {==}

# write_carries_user
# ------------------
# The hypothesis w : {is_write(classify(req)) == True{}} is a false equation
# for every decision that is a reject or a read. The proof splits req into
# 21 cases, and false_ne_true closes 19 of them. In the other two, a User
# posts (Trimmed body) or likes (ValidId), the decision is
# Run{InsertPost{uid, ..}} or Run{InsertLike{uid, ..}}, op_uid is Some{uid},
# and so is auth_uid of the request: both sides compute and {==} closes it.

def Goal.write_carries_user(+req: Req.Request) -> Type:
  {Req.op_uid(Req.classify(req)) == Req.auth_uid(req) : Maybe<U32>}

def Laws.write_carries_user(req, w):
  match req:
    case Req.Request{route, auth, body}:
      match route:
        case Req.Health{}:
          false_ne_true(Goal.write_carries_user(Req.Request{Req.Health{}, auth, body}), w)
        case Req.Feed{}:
          false_ne_true(Goal.write_carries_user(Req.Request{Req.Feed{}, auth, body}), w)
        case Req.PostById{pid}:
          match pid:
            case Req.ValidId{id}:
              false_ne_true(Goal.write_carries_user(Req.Request{Req.PostById{Req.ValidId{id}}, auth, body}), w)
            case Req.InvalidId{}:
              false_ne_true(Goal.write_carries_user(Req.Request{Req.PostById{Req.InvalidId{}}, auth, body}), w)
        case Req.CreatePost{}:
          match auth:
            case Req.User{uid, name}:
              match body:
                case Req.Trimmed{s}:
                  {==}
                case Req.TooLong{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.User{uid, name}, Req.TooLong{}}), w)
                case Req.Missing{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.User{uid, name}, Req.Missing{}}), w)
                case Req.NoneSent{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.User{uid, name}, Req.NoneSent{}}), w)
                case Req.NotJson{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.User{uid, name}, Req.NotJson{}}), w)
            case Req.BadPayload{}:
              false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.BadPayload{}, body}), w)
            case Req.BadToken{}:
              false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.BadToken{}, body}), w)
            case Req.NoBearer{}:
              false_ne_true(Goal.write_carries_user(Req.Request{Req.CreatePost{}, Req.NoBearer{}, body}), w)
        case Req.LikePost{pid}:
          match pid:
            case Req.ValidId{id}:
              match auth:
                case Req.User{uid, name}:
                  {==}
                case Req.BadPayload{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.ValidId{id}}, Req.BadPayload{}, body}), w)
                case Req.BadToken{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.ValidId{id}}, Req.BadToken{}, body}), w)
                case Req.NoBearer{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.ValidId{id}}, Req.NoBearer{}, body}), w)
            case Req.InvalidId{}:
              match auth:
                case Req.User{uid, name}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.InvalidId{}}, Req.User{uid, name}, body}), w)
                case Req.BadPayload{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.InvalidId{}}, Req.BadPayload{}, body}), w)
                case Req.BadToken{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.InvalidId{}}, Req.BadToken{}, body}), w)
                case Req.NoBearer{}:
                  false_ne_true(Goal.write_carries_user(Req.Request{Req.LikePost{Req.InvalidId{}}, Req.NoBearer{}, body}), w)
        case Req.Unknown{}:
          false_ne_true(Goal.write_carries_user(Req.Request{Req.Unknown{}, auth, body}), w)

# Non-vacuity of the hypotheses
# -----------------------------
# Each law with a hypothesis is applied at the example of
# tests/classify_examples.bend, so every hypothesis is inhabited there: a
# law whose hypothesis nothing satisfied would prove nothing.

def Witness.auth_gate() -> Goal.auth_gate(Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}):
  Laws.auth_gate(Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}, {==}, {==})

def Witness.write_carries_user() -> Goal.write_carries_user(Req.Request{Req.LikePost{Req.ValidId{7}}, Req.User{3, "ann"}, Req.NoneSent{}}):
  Laws.write_carries_user(Req.Request{Req.LikePost{Req.ValidId{7}}, Req.User{3, "ann"}, Req.NoneSent{}}, {==})

def Witness.bad_body_no_write() -> Goal.bad_body_no_write(Req.User{3, "ann"}, Req.TooLong{}):
  Laws.bad_body_no_write(Req.User{3, "ann"}, Req.TooLong{}, {==})

# S02: the response policy
# ========================
# respond matches the operation and the outcome together. Its rows name
# the one success outcome of each operation; every other outcome reaches
# the row after it, which is an error. A proof fixes the operation (or
# splits it), then splits the outcome constructor by constructor, until
# respond computes. Then {==} closes the goal, or the hypothesis has become
# a false equation between True{} and False{} and false_ne_true or
# true_ne_false refutes the case. The hypotheses name one outcome
# constructor each, so each refutes exactly one case.

# created_needs_commit
# --------------------
# op is InsertPost. Of the eight outcomes only Committed gives Created; the
# hypothesis h : {is_committed(out) == False{}} is {True{} == False{}} there.
# The seven others reach the 500 row, and is_success of a 500 is False{}.

def Goal.created_needs_commit(u: U32, uid: U32, name: String, text: String, out: Rsp.Outcome) -> Type:
  {Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, out, u)) == False{} : Bool}

def Laws.created_needs_commit(u, uid, name, text, out, h):
  match out:
    case Rsp.Committed{post_id, created_at}:
      true_ne_false(Goal.created_needs_commit(u, uid, name, text, Rsp.Committed{post_id, created_at}), h)
    case Rsp.Liked{already}:
      {==}
    case Rsp.Post{post}:
      {==}
    case Rsp.Posts{posts}:
      {==}
    case Rsp.Ping{}:
      {==}
    case Rsp.NotFound{}:
      {==}
    case Rsp.Unknown{}:
      {==}
    case Rsp.Failed{}:
      {==}

# liked_needs_commit
# ------------------
# op is InsertLike. Liked{already} is refuted by h whatever already is,
# because is_liked does not read it. respond splits already only in the
# cases the proof never reaches, so the proof does not split it.
# NotFound gives 404 and the six others 500: both are below success.

def Goal.liked_needs_commit(u: U32, uid: U32, pid: U32, out: Rsp.Outcome) -> Type:
  {Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, out, u)) == False{} : Bool}

def Laws.liked_needs_commit(u, uid, pid, out, h):
  match out:
    case Rsp.Committed{post_id, created_at}:
      {==}
    case Rsp.Liked{already}:
      true_ne_false(Goal.liked_needs_commit(u, uid, pid, Rsp.Liked{already}), h)
    case Rsp.Post{post}:
      {==}
    case Rsp.Posts{posts}:
      {==}
    case Rsp.Ping{}:
      {==}
    case Rsp.NotFound{}:
      {==}
    case Rsp.Unknown{}:
      {==}
    case Rsp.Failed{}:
      {==}

# no_success_on_unknown, unknown_is_500
# -------------------------------------
# The outcome is Unknown{}, so respond computes once the operation is
# known. SelectOne gives Degraded (503), the four others 500. Both are
# below 400, so is_success is False{} in all five cases. unknown_is_500
# adds the hypothesis is_select_one(op) == False{}, which refutes SelectOne.

def Laws.no_success_on_unknown(op, u):
  match op:
    case Req.SelectOne{}:
      {==}
    case Req.FeedQuery{}:
      {==}
    case Req.PostQuery{pid}:
      {==}
    case Req.InsertPost{uid, name, text}:
      {==}
    case Req.InsertLike{uid, pid}:
      {==}

def Goal.unknown_is_500(op: Req.Op, u: U32) -> Type:
  {Rsp.reply_status(Rsp.respond(op, Rsp.Unknown{}, u)) == 500 : U32}

def Laws.unknown_is_500(op, u, h):
  match op:
    case Req.SelectOne{}:
      true_ne_false(Goal.unknown_is_500(Req.SelectOne{}, u), h)
    case Req.FeedQuery{}:
      {==}
    case Req.PostQuery{pid}:
      {==}
    case Req.InsertPost{uid, name, text}:
      {==}
    case Req.InsertLike{uid, pid}:
      {==}

# health_unknown_is_503
# ---------------------
# op is SelectOne. Only Ping gives Health; h refutes that case. The seven
# others reach the Degraded row, whose status is 503.

def Goal.health_unknown_is_503(u: U32, out: Rsp.Outcome) -> Type:
  {Rsp.reply_status(Rsp.respond(Req.SelectOne{}, out, u)) == 503 : U32}

def Laws.health_unknown_is_503(u, out, h):
  match out:
    case Rsp.Committed{post_id, created_at}:
      {==}
    case Rsp.Liked{already}:
      {==}
    case Rsp.Post{post}:
      {==}
    case Rsp.Posts{posts}:
      {==}
    case Rsp.Ping{}:
      true_ne_false(Goal.health_unknown_is_503(u, Rsp.Ping{}), h)
    case Rsp.NotFound{}:
      {==}
    case Rsp.Unknown{}:
      {==}
    case Rsp.Failed{}:
      {==}

# not_found_is_404, created_echoes_request, like_status
# -----------------------------------------------------
# These name the operation and the outcome outright, so respond computes
# and every equation is closed by {==}. like_status and not_found_is_404
# are pairs of equations, one {==} for each.

def Laws.not_found_is_404(id, uid, u):
  ({==}, {==})

def Laws.created_echoes_request(u, uid, name, text, id, ts):
  {==}

def Laws.like_status(uid, pid, u):
  ({==}, {==})

# health_ok_reports_uptime, feed_ok_passes_rows, post_ok_passes_row
# -----------------------------------------------------------------
# Each names its operation and its success outcome outright, so respond
# computes to the one row that builds the reply, and {==} closes the goal.
# These are the three success rows that no other law reads: without them a
# respond that answers Health{0} when uptime_s is 99 passes every proof.

def Laws.health_ok_reports_uptime(u):
  {==}

def Laws.feed_ok_passes_rows(u, ps):
  {==}

def Laws.post_ok_passes_row(pid, u, p):
  {==}

# created_only_for_insert_post, liked_only_for_insert_like
# --------------------------------------------------------
# The goal is an equation between two Bools. respond matches the op and the
# outcome together, so the proof splits the op into its five constructors
# and the outcome into its eight, 40 leaves, and in each leaf respond,
# is_created (or is_liked_reply), is_insert_post (or is_insert_like),
# is_committed (or is_liked) and Bool.and all compute, so {==} closes it.
# The one extra split is already, under InsertLike x Liked: respond
# matches it, to give 201 or 200.

def Laws.created_only_for_insert_post(op, out, u):
  match op:
    case Req.SelectOne{}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.FeedQuery{}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.PostQuery{pid}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.InsertPost{uid, name, text}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.InsertLike{uid, pid}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          match already:
            case False{}:
              {==}
            case True{}:
              {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}

def Laws.liked_only_for_insert_like(op, out, u):
  match op:
    case Req.SelectOne{}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.FeedQuery{}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.PostQuery{pid}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.InsertPost{uid, name, text}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}
    case Req.InsertLike{uid, pid}:
      match out:
        case Rsp.Committed{post_id, created_at}:
          {==}
        case Rsp.Liked{already}:
          match already:
            case False{}:
              {==}
            case True{}:
              {==}
        case Rsp.Post{post}:
          {==}
        case Rsp.Posts{posts}:
          {==}
        case Rsp.Ping{}:
          {==}
        case Rsp.NotFound{}:
          {==}
        case Rsp.Unknown{}:
          {==}
        case Rsp.Failed{}:
          {==}

# Non-vacuity of the hypotheses
# -----------------------------
# Each S02 law with a hypothesis is applied at a concrete outcome for
# which the hypothesis holds, so {==} proves the hypothesis there. The
# first two use the other write's success outcome: Liked does not
# authorise a create, nor Committed a like. The next two use outcomes
# that a host does produce for the same ops: Unknown for an InsertPost
# whose connection died before ReadyForQuery, and NotFound for an
# InsertLike that hit SQLSTATE 23503.

def Witness.created_needs_commit() -> Goal.created_needs_commit(7, 3, "ann", "hi", Rsp.Liked{True{}}):
  Laws.created_needs_commit(7, 3, "ann", "hi", Rsp.Liked{True{}}, {==})

def Witness.liked_needs_commit() -> Goal.liked_needs_commit(7, 3, 9, Rsp.Committed{9, "t"}):
  Laws.liked_needs_commit(7, 3, 9, Rsp.Committed{9, "t"}, {==})

def Witness.health_unknown_is_503() -> Goal.health_unknown_is_503(7, Rsp.Unknown{}):
  Laws.health_unknown_is_503(7, Rsp.Unknown{}, {==})

def Witness.unknown_is_500() -> Goal.unknown_is_500(Req.FeedQuery{}, 7):
  Laws.unknown_is_500(Req.FeedQuery{}, 7, {==})

def Witness.created_needs_commit_unknown() -> Goal.created_needs_commit(7, 3, "ann", "hi", Rsp.Unknown{}):
  Laws.created_needs_commit(7, 3, "ann", "hi", Rsp.Unknown{}, {==})

def Witness.liked_needs_commit_not_found() -> Goal.liked_needs_commit(7, 3, 9, Rsp.NotFound{}):
  Laws.liked_needs_commit(7, 3, 9, Rsp.NotFound{}, {==})

# S07 and S10: the command policy
# ===============================
# The five trace laws are proved by induction on the event list in
# run.go(es, t, acc). t is the last step's result (s, c) and acc the
# commands before it; the recursive call is the hypothesis for the rest of
# the list, with t := step(s, e) and acc := acc ++ c. Each trace law has an
# invariant Inv(t, acc) and a per-step lemma
# Inv((s, c), acc) -> Inv(step(s, e), acc ++ c) that splits s and e; the
# run lemma never splits an event. failed_batch_no_success runs two such
# inductions, one over the events before the failure and one over the
# events after it; gone_no_reply runs one over the events after the Gone.
# The two per-event laws need no invariant: their proofs split the state
# and induct on its batches.
#
# Since S10 the state holds the two open batches (reads, writes) and the
# batches in flight, each batch with its own entries. Flushed, Synced and
# Died compute hr and hw: whether the open read batch, and the open write
# batch, has the event's id. The per-step lemmas take hr and hw as
# parameters and split them only where a count depends on which batches
# moved.
#
# Names: the lemmas of a law's proof are named <law>.<lemma> and are used by
# that proof only. The lemmas shared by several proofs (Bool, U32, list
# membership, counts) come first and do not mention src/policy.bend.
# scripts/verify.sh --regress checks this naming, keeps for each mutant
# only the proof of the law the mutant names and the defs it uses, and
# requires that proof to fail inside Laws.<law> or a <law>.* lemma.

# Bool facts
# ----------

def or_true_r(+a: Bool) -> {Bool.or(a, True{}) == True{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      {==}

def implies_true_r(+a: Bool) -> {Laws.implies(a, True{}) == True{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      {==}

# U32 equality is reflexive
# -------------------------
# Base defines U32.is_eq(a, b) as Cmp.is_eq(U32.cmp(a, b)), a comparison of
# 32-bit words bit by bit, and proves nothing about it. word_cmp_refl walks
# the word: each bit compares equal to itself and Word.cmp.fin keeps the EQ{}
# of the rest. Only reflexivity is needed: every proof below compares the
# same two terms that the kernel compares.

def word_cmp_refl(n: Nat, +a: Word(n)) -> {Word.cmp(n, a, a) == EQ{} : Cmp}:
  match n:
    case 0n:
      {==}
    case 1n+p:
      match a:
        case WCon{ab, at}:
          match ab:
            case False{}:
              %Equal.sym(Cmp, Word.cmp(p, at, at), EQ{}, word_cmp_refl(p, at)) : {Word.cmp.fin(False{}, False{}, _) == EQ{} : Cmp}
              {==}
            case True{}:
              %Equal.sym(Cmp, Word.cmp(p, at, at), EQ{}, word_cmp_refl(p, at)) : {Word.cmp.fin(True{}, True{}, _) == EQ{} : Cmp}
              {==}

def u32_eq_refl(+a: U32) -> {U32.is_eq(a, a) == True{} : Bool}:
  match a:
    case U32{w}:
      %Equal.sym(Cmp, Word.cmp(32n, w, w), EQ{}, word_cmp_refl(32n, w)) : {Cmp.is_eq(_) == True{} : Bool}
      {==}

# More Bool facts
# ---------------

def and_l(+a: Bool, -b: Bool, h: {Bool.and(a, b) == True{} : Bool}) -> {a == True{} : Bool}:
  match a:
    case False{}:
      false_ne_true({False{} == True{} : Bool}, h)
    case True{}:
      {==}

def and_r(+a: Bool, -b: Bool, h: {Bool.and(a, b) == True{} : Bool}) -> {b == True{} : Bool}:
  match a:
    case False{}:
      false_ne_true({b == True{} : Bool}, h)
    case True{}:
      h

def and_intro(-a: Bool, -b: Bool, ha: {a == True{} : Bool}, hb: {b == True{} : Bool}) -> {Bool.and(a, b) == True{} : Bool}:
  %Equal.sym(Bool, a, True{}, ha) : {Bool.and(_, b) == True{} : Bool}
  hb

def and_assoc(+a: Bool, -b: Bool, -c: Bool) -> {Bool.and(Bool.and(a, b), c) == Bool.and(a, Bool.and(b, c)) : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      {==}

def implies_mp(+a: Bool, -b: Bool, h: {Laws.implies(a, b) == True{} : Bool}, ha: {a == True{} : Bool}) -> {b == True{} : Bool}:
  match a:
    case False{}:
      false_ne_true({b == True{} : Bool}, ha)
    case True{}:
      h

def not_true(+a: Bool, h: {Bool.not(a) == True{} : Bool}) -> {a == False{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      false_ne_true({True{} == False{} : Bool}, h)

# List membership
# ---------------
# Mem(A, x, xs) is a proof that x is an element of xs: Empty for Nil{}, and
# for h <> t either h is x or x is in t. AllIn(A, xs, ys) says every element
# of xs is in ys. The trace proofs use AllIn(es, whole) to know that each event
# of a suffix es is an event of the whole trace.

def Mem(-A: Data, x: A, xs: List<&2, A>) -> Data:
  match xs:
    case Nil{}:
      Empty
    case h <> t:
      Either<&2, &2, {h == x : A}, Mem(A, x, t)>

def mem_append_l(-A: Data, -x: A, xs: List<&2, A>, -ys: List<&2, A>, m: Mem(A, x, xs)) -> Mem(A, x, List.append(&2, A, xs, ys)):
  match xs:
    case Nil{}:
      match m:
    case h <> t:
      match m:
        case Inl{e}:
          Inl{e}
        case Inr{n}:
          Inr{mem_append_l(A, x, t, ys, n)}

def mem_append_r(-A: Data, -x: A, xs: List<&2, A>, -ys: List<&2, A>, m: Mem(A, x, ys)) -> Mem(A, x, List.append(&2, A, xs, ys)):
  match xs:
    case Nil{}:
      m
    case h <> t:
      Inr{mem_append_r(A, x, t, ys, m)}

def AllIn(-A: Data, xs: List<&2, A>, +ys: List<&2, A>) -> Data:
  match xs:
    case Nil{}:
      Unit
    case x <> rest:
      +Sigma<Mem(A, x, ys), _ => AllIn(A, rest, ys)>

def all_in_cons(-A: Data, xs: List<&2, A>, -y: A, -ys: List<&2, A>, h: AllIn(A, xs, ys)) -> AllIn(A, xs, y <> ys):
  match xs:
    case Nil{}:
      Unit{}
    case x <> rest:
      match h:
        case (m, hr):
          (Inr{m}, all_in_cons(A, rest, y, ys, hr))

def all_in_self(-A: Data, +xs: List<&2, A>) -> AllIn(A, xs, xs):
  match xs:
    case Nil{}:
      Unit{}
    case x <> rest:
      (Inl{{==}}, all_in_cons(A, rest, x, rest, all_in_self(A, rest)))

def implies_or_l(+s: Bool, -a: Bool, -b: Bool, h: {Laws.implies(s, a) == True{} : Bool}) -> {Laws.implies(s, Bool.or(a, b)) == True{} : Bool}:
  match s:
    case False{}:
      {==}
    case True{}:
      %Equal.sym(Bool, a, True{}, h) : {Bool.or(_, b) == True{} : Bool}
      {==}

def implies_or_r(+s: Bool, +a: Bool, -b: Bool, h: {Laws.implies(s, b) == True{} : Bool}) -> {Laws.implies(s, Bool.or(a, b)) == True{} : Bool}:
  match s:
    case False{}:
      {==}
    case True{}:
      %Equal.sym(Bool, b, True{}, h) : {Bool.or(a, _) == True{} : Bool}
      or_true_r(a)

# Counts
# ------
# Count is Laws' saturating count: Never, Once, Repeated. le is its order.
# Each fact below splits every variable; at each leaf the terms compute, and
# {==} closes it or a hypothesis has become False{} == True{}.

def count.le(a: Laws.Count, b: Laws.Count) -> Bool:
  match a:
    case Laws.Never{}:
      True{}
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          False{}
        case Laws.Once{}:
          True{}
        case Laws.Repeated{}:
          True{}
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          False{}
        case Laws.Once{}:
          False{}
        case Laws.Repeated{}:
          True{}

# Saturating addition is associative ...
def Goal.count.assoc(a: Laws.Count, b: Laws.Count, c: Laws.Count) -> Type:
  {Laws.count.add(Laws.count.add(a, b), c) == Laws.count.add(a, Laws.count.add(b, c)) : Laws.Count}

def count.assoc(a: Laws.Count, b: Laws.Count, c: Laws.Count) -> Goal.count.assoc(a, b, c):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}

# ... and its two left summands commute.
def Goal.count.swap(a: Laws.Count, b: Laws.Count, c: Laws.Count) -> Type:
  {Laws.count.add(a, Laws.count.add(b, c)) == Laws.count.add(b, Laws.count.add(a, c)) : Laws.Count}

def count.swap(a: Laws.Count, b: Laws.Count, c: Laws.Count) -> Goal.count.swap(a, b, c):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}

# A smaller middle summand keeps a sum at most Once.
def Goal.count.mono(x: Laws.Count, m: Laws.Count, n: Laws.Count, y: Laws.Count) -> Type:
  {Laws.at_most_once(Laws.count.add(Laws.count.add(x, m), y)) == True{} : Bool}

def count.mono(x: Laws.Count, m: Laws.Count, n: Laws.Count, y: Laws.Count, hle: {count.le(m, n) == True{} : Bool}, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(x, n), y)) == True{} : Bool}) -> Goal.count.mono(x, m, n, y):
  match x:
    case Laws.Never{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Once{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), hle)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Never{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Once{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Once{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), hle)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), hle)
            case Laws.Once{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), hle)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}, Laws.Once{}), hle)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), hle)
            case Laws.Repeated{}:
              match y:
                case Laws.Never{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
                case Laws.Once{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)

# A part of a sum that is at most Once is at most Once.
def Goal.count.base(x: Laws.Count, y: Laws.Count) -> Type:
  {Laws.at_most_once(x) == True{} : Bool}

def count.base(x: Laws.Count, y: Laws.Count, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(x, y), Laws.Never{})) == True{} : Bool}) -> Goal.count.base(x, y):
  match x:
    case Laws.Never{}:
      match y:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          false_ne_true(Goal.count.base(Laws.Never{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match y:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.base(Laws.Once{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.base(Laws.Once{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match y:
        case Laws.Never{}:
          false_ne_true(Goal.count.base(Laws.Repeated{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.base(Laws.Repeated{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.base(Laws.Repeated{}, Laws.Repeated{}), h)

# A rejected arrival: one reply for it, pending unchanged.
def Goal.count.arr_reject(h: Bool, p: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.add(Laws.count.of(h), Laws.Never{}), p), Laws.count.add(p, Laws.count.of(h))) == True{} : Bool}

def count.arr_reject(h: Bool, p: Laws.Count) -> Goal.count.arr_reject(h, p):
  match h:
    case False{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}
    case True{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}

# A run arrival: one pending entry for it, no reply.
def Goal.count.arr_run(h: Bool, p: Laws.Count) -> Type:
  {count.le(Laws.count.add(p, Laws.count.add(Laws.count.of(h), Laws.Never{})), Laws.count.add(p, Laws.count.of(h))) == True{} : Bool}

def count.arr_run(h: Bool, p: Laws.Count) -> Goal.count.arr_run(h, p):
  match h:
    case False{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}
    case True{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}

# An event that moves entries to replies or changes nothing.
def Goal.count.same(p: Laws.Count) -> Type:
  {count.le(p, Laws.count.add(p, Laws.Never{})) == True{} : Bool}

def count.same(p: Laws.Count) -> Goal.count.same(p):
  match p:
    case Laws.Never{}:
      {==}
    case Laws.Once{}:
      {==}
    case Laws.Repeated{}:
      {==}

# Gone: fewer pending entries, no reply.
def Goal.count.gone(m: Laws.Count, n: Laws.Count) -> Type:
  {count.le(m, Laws.count.add(n, Laws.Never{})) == True{} : Bool}

def count.gone(m: Laws.Count, n: Laws.Count, hle: {count.le(m, n) == True{} : Bool}) -> Goal.count.gone(m, n):
  match m:
    case Laws.Never{}:
      match n:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}
    case Laws.Once{}:
      match n:
        case Laws.Never{}:
          false_ne_true(Goal.count.gone(Laws.Once{}, Laws.Never{}), hle)
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}
    case Laws.Repeated{}:
      match n:
        case Laws.Never{}:
          false_ne_true(Goal.count.gone(Laws.Repeated{}, Laws.Never{}), hle)
        case Laws.Once{}:
          false_ne_true(Goal.count.gone(Laws.Repeated{}, Laws.Once{}), hle)
        case Laws.Repeated{}:
          {==}

# le(m, n) survives adding to the right side ...
def Goal.count.grow(x: Laws.Count, m: Laws.Count, n: Laws.Count) -> Type:
  {count.le(m, Laws.count.add(x, n)) == True{} : Bool}

def count.grow(x: Laws.Count, m: Laws.Count, n: Laws.Count, hle: {count.le(m, n) == True{} : Bool}) -> Goal.count.grow(x, m, n):
  match x:
    case Laws.Never{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Never{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.grow(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Once{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.grow(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.grow(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.grow(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}

# ... and adding the same summand to both sides.
def Goal.count.both(x: Laws.Count, m: Laws.Count, n: Laws.Count) -> Type:
  {count.le(Laws.count.add(x, m), Laws.count.add(x, n)) == True{} : Bool}

def count.both(x: Laws.Count, m: Laws.Count, n: Laws.Count, hle: {count.le(m, n) == True{} : Bool}) -> Goal.count.both(x, m, n):
  match x:
    case Laws.Never{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Never{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.both(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Once{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.both(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match m:
        case Laws.Never{}:
          match n:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), hle)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match n:
            case Laws.Never{}:
              false_ne_true(Goal.count.both(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), hle)
            case Laws.Once{}:
              false_ne_true(Goal.count.both(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), hle)
            case Laws.Repeated{}:
              {==}

# A sum at most Once whose first summand is at least Once: the second
# summand is Never ...
def Goal.count.pinch_p(q: Laws.Count, p: Laws.Count, a: Laws.Count) -> Type:
  {count.le(p, Laws.Never{}) == True{} : Bool}

def count.pinch_p(q: Laws.Count, p: Laws.Count, a: Laws.Count, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(q, p), a)) == True{} : Bool}, hq: {count.le(Laws.Once{}, q) == True{} : Bool}) -> Goal.count.pinch_p(q, p, a):
  match q:
    case Laws.Never{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Never{}, Laws.Never{}), hq)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Never{}, Laws.Once{}), hq)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Once{}, Laws.Never{}), hq)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Never{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_p(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)

# ... and so is the third.
def Goal.count.pinch_a(q: Laws.Count, p: Laws.Count, a: Laws.Count) -> Type:
  {count.le(a, Laws.Never{}) == True{} : Bool}

def count.pinch_a(q: Laws.Count, p: Laws.Count, a: Laws.Count, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(q, p), a)) == True{} : Bool}, hq: {count.le(Laws.Once{}, q) == True{} : Bool}) -> Goal.count.pinch_a(q, p, a):
  match q:
    case Laws.Never{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Never{}, Laws.Never{}), hq)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Never{}, Laws.Once{}), hq)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Once{}, Laws.Never{}), hq)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Never{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.pinch_a(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)

# A Never sum has a Never tail.
def Goal.count.never_tail(x: Bool, q: Laws.Count) -> Type:
  {count.le(q, Laws.Never{}) == True{} : Bool}

def count.never_tail(x: Bool, q: Laws.Count, h: {count.le(Laws.count.add(Laws.count.of(x), q), Laws.Never{}) == True{} : Bool}) -> Goal.count.never_tail(x, q):
  match x:
    case False{}:
      match q:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.never_tail(False{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_tail(False{}, Laws.Repeated{}), h)
    case True{}:
      match q:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_tail(True{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_tail(True{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_tail(True{}, Laws.Repeated{}), h)

# A Never sum has a False head: x implies anything.
def Goal.count.implies_never(x: Bool, y: Bool, q: Laws.Count) -> Type:
  {Laws.implies(x, y) == True{} : Bool}

def count.implies_never(x: Bool, y: Bool, q: Laws.Count, h: {count.le(Laws.count.add(Laws.count.of(x), q), Laws.Never{}) == True{} : Bool}) -> Goal.count.implies_never(x, y, q):
  match x:
    case False{}:
      match y:
        case False{}:
          match q:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.implies_never(False{}, False{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.implies_never(False{}, False{}, Laws.Repeated{}), h)
        case True{}:
          match q:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.implies_never(False{}, True{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.implies_never(False{}, True{}, Laws.Repeated{}), h)
    case True{}:
      match y:
        case False{}:
          match q:
            case Laws.Never{}:
              false_ne_true(Goal.count.implies_never(True{}, False{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.implies_never(True{}, False{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.implies_never(True{}, False{}, Laws.Repeated{}), h)
        case True{}:
          match q:
            case Laws.Never{}:
              false_ne_true(Goal.count.implies_never(True{}, True{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.implies_never(True{}, True{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.implies_never(True{}, True{}, Laws.Repeated{}), h)

# An arrival that is not for u adds no entry for u.
def Goal.count.run_none(x: Bool, p: Laws.Count, a: Laws.Count) -> Type:
  {count.le(Laws.count.add(p, Laws.count.add(Laws.count.of(x), Laws.Never{})), Laws.Never{}) == True{} : Bool}

def count.run_none(x: Bool, p: Laws.Count, a: Laws.Count, hp: {count.le(p, Laws.Never{}) == True{} : Bool}, ha: {count.le(Laws.count.add(Laws.count.of(x), a), Laws.Never{}) == True{} : Bool}) -> Goal.count.run_none(x, p, a):
  match x:
    case False{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Never{}, Laws.Once{}), ha)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Never{}, Laws.Repeated{}), ha)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Once{}, Laws.Never{}), hp)
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Once{}, Laws.Once{}), hp)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Once{}, Laws.Repeated{}), hp)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Repeated{}, Laws.Never{}), hp)
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Repeated{}, Laws.Once{}), hp)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(False{}, Laws.Repeated{}, Laws.Repeated{}), hp)
    case True{}:
      match p:
        case Laws.Never{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Never{}, Laws.Never{}), ha)
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Never{}, Laws.Once{}), ha)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Never{}, Laws.Repeated{}), ha)
        case Laws.Once{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Once{}, Laws.Never{}), hp)
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Once{}, Laws.Once{}), hp)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Once{}, Laws.Repeated{}), hp)
        case Laws.Repeated{}:
          match a:
            case Laws.Never{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Repeated{}, Laws.Never{}), hp)
            case Laws.Once{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Repeated{}, Laws.Once{}), hp)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.run_none(True{}, Laws.Repeated{}, Laws.Repeated{}), hp)

# Below Never is Never.
def Goal.count.trans_never(m: Laws.Count, n: Laws.Count) -> Type:
  {count.le(m, Laws.Never{}) == True{} : Bool}

def count.trans_never(m: Laws.Count, n: Laws.Count, hle: {count.le(m, n) == True{} : Bool}, hn: {count.le(n, Laws.Never{}) == True{} : Bool}) -> Goal.count.trans_never(m, n):
  match m:
    case Laws.Never{}:
      match n:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.trans_never(Laws.Never{}, Laws.Once{}), hn)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.trans_never(Laws.Never{}, Laws.Repeated{}), hn)
    case Laws.Once{}:
      match n:
        case Laws.Never{}:
          false_ne_true(Goal.count.trans_never(Laws.Once{}, Laws.Never{}), hle)
        case Laws.Once{}:
          false_ne_true(Goal.count.trans_never(Laws.Once{}, Laws.Once{}), hn)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.trans_never(Laws.Once{}, Laws.Repeated{}), hn)
    case Laws.Repeated{}:
      match n:
        case Laws.Never{}:
          false_ne_true(Goal.count.trans_never(Laws.Repeated{}, Laws.Never{}), hle)
        case Laws.Once{}:
          false_ne_true(Goal.count.trans_never(Laws.Repeated{}, Laws.Once{}), hle)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.trans_never(Laws.Repeated{}, Laws.Repeated{}), hn)

# An entry outside batch b is not for u.
def Goal.count.keep_none(x: Bool, q: Laws.Count, p: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.of(x), p), Laws.Never{}) == True{} : Bool}

def count.keep_none(x: Bool, q: Laws.Count, p: Laws.Count, hh: {count.le(Laws.count.add(Laws.count.of(Bool.and(x, True{})), q), Laws.Never{}) == True{} : Bool}, ih: {count.le(p, Laws.Never{}) == True{} : Bool}) -> Goal.count.keep_none(x, q, p):
  match x:
    case False{}:
      match q:
        case Laws.Never{}:
          match p:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Never{}, Laws.Once{}), ih)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Never{}, Laws.Repeated{}), ih)
        case Laws.Once{}:
          match p:
            case Laws.Never{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Once{}, Laws.Never{}), hh)
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Once{}, Laws.Once{}), hh)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Once{}, Laws.Repeated{}), hh)
        case Laws.Repeated{}:
          match p:
            case Laws.Never{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Repeated{}, Laws.Never{}), hh)
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Repeated{}, Laws.Once{}), hh)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(False{}, Laws.Repeated{}, Laws.Repeated{}), hh)
    case True{}:
      match q:
        case Laws.Never{}:
          match p:
            case Laws.Never{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Never{}, Laws.Never{}), hh)
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Never{}, Laws.Once{}), hh)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Never{}, Laws.Repeated{}), hh)
        case Laws.Once{}:
          match p:
            case Laws.Never{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Once{}, Laws.Never{}), hh)
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Once{}, Laws.Once{}), hh)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Once{}, Laws.Repeated{}), hh)
        case Laws.Repeated{}:
          match p:
            case Laws.Never{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Repeated{}, Laws.Never{}), hh)
            case Laws.Once{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Repeated{}, Laws.Once{}), hh)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.keep_none(True{}, Laws.Repeated{}, Laws.Repeated{}), hh)

# A Queue found in a list is counted in it.
def Goal.count.queued_step(x: Bool, y: Bool, q: Laws.Count) -> Type:
  {Laws.implies(Bool.or(x, y), count.le(Laws.Once{}, Laws.count.add(Laws.count.of(x), q))) == True{} : Bool}

def count.queued_step(x: Bool, y: Bool, q: Laws.Count, ih: {Laws.implies(y, count.le(Laws.Once{}, q)) == True{} : Bool}) -> Goal.count.queued_step(x, y, q):
  match x:
    case False{}:
      match y:
        case False{}:
          match q:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case True{}:
          match q:
            case Laws.Never{}:
              false_ne_true(Goal.count.queued_step(False{}, True{}, Laws.Never{}), ih)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case True{}:
      match y:
        case False{}:
          match q:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case True{}:
          match q:
            case Laws.Never{}:
              false_ne_true(Goal.count.queued_step(True{}, True{}, Laws.Never{}), ih)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}

# le is reflexive.
def Goal.count.le_refl(p: Laws.Count) -> Type:
  {count.le(p, p) == True{} : Bool}

def count.le_refl(p: Laws.Count) -> Goal.count.le_refl(p):
  match p:
    case Laws.Never{}:
      {==}
    case Laws.Once{}:
      {==}
    case Laws.Repeated{}:
      {==}

# A rejected arrival: no Queue, no entry.
def Goal.count.le_add_r(x: Bool, p: Laws.Count) -> Type:
  {count.le(p, Laws.count.add(p, Laws.count.of(x))) == True{} : Bool}

def count.le_add_r(x: Bool, p: Laws.Count) -> Goal.count.le_add_r(x, p):
  match x:
    case False{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}
    case True{}:
      match p:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          {==}
        case Laws.Repeated{}:
          {==}

# A run arrival for u: a Queue for u in b, or an entry for u outside b.
def Goal.count.run_split(x: Bool, y: Bool, p: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.add(Laws.count.of(Bool.and(x, y)), Laws.Never{}), Laws.count.add(p, Laws.count.add(Laws.count.of(Bool.and(y, Bool.not(x))), Laws.Never{}))), Laws.count.add(p, Laws.count.of(y))) == True{} : Bool}

def count.run_split(x: Bool, y: Bool, p: Laws.Count) -> Goal.count.run_split(x, y, p):
  match x:
    case False{}:
      match y:
        case False{}:
          match p:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case True{}:
          match p:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case True{}:
      match y:
        case False{}:
          match p:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case True{}:
          match p:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}

# S10: more Bool facts
# --------------------

def or_false_l(+a: Bool, -b: Bool, h: {Bool.or(a, b) == False{} : Bool}) -> {a == False{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      true_ne_false({True{} == False{} : Bool}, h)

def or_false_r(+a: Bool, -b: Bool, h: {Bool.or(a, b) == False{} : Bool}) -> {b == False{} : Bool}:
  match a:
    case False{}:
      h
    case True{}:
      true_ne_false({b == False{} : Bool}, h)

def not_false(+a: Bool, h: {a == False{} : Bool}) -> {Bool.not(a) == True{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      true_ne_false({Bool.not(True{}) == True{} : Bool}, h)

def implies_false(+a: Bool, -b: Bool, h: {a == False{} : Bool}) -> {Laws.implies(a, b) == True{} : Bool}:
  match a:
    case False{}:
      {==}
    case True{}:
      true_ne_false({Laws.implies(True{}, b) == True{} : Bool}, h)

def and3_intro(-a: Bool, -b: Bool, -c: Bool, ha: {a == True{} : Bool}, hb: {b == True{} : Bool}, hc: {c == True{} : Bool}) -> {Bool.and(Bool.and(a, b), c) == True{} : Bool}:
  and_intro(Bool.and(a, b), c, and_intro(a, b, ha, hb), hc)

def and3_1(+a: Bool, +b: Bool, -c: Bool, h: {Bool.and(Bool.and(a, b), c) == True{} : Bool}) -> {a == True{} : Bool}:
  and_l(a, b, and_l(Bool.and(a, b), c, h))

def and3_2(+a: Bool, +b: Bool, -c: Bool, h: {Bool.and(Bool.and(a, b), c) == True{} : Bool}) -> {b == True{} : Bool}:
  and_r(a, b, and_l(Bool.and(a, b), c, h))

def and3_3(+a: Bool, +b: Bool, -c: Bool, h: {Bool.and(Bool.and(a, b), c) == True{} : Bool}) -> {c == True{} : Bool}:
  and_r(Bool.and(a, b), c, h)

# S10: more count facts
# ---------------------
# Brute force as above, except the last few, which rewrite with an
# equation they are given.

# Sums are monotone in both summands.
def Goal.count.add_mono(a: Laws.Count, b: Laws.Count, c: Laws.Count, d: Laws.Count) -> Type:
  {count.le(Laws.count.add(a, c), Laws.count.add(b, d)) == True{} : Bool}

def count.add_mono(a: Laws.Count, b: Laws.Count, c: Laws.Count, d: Laws.Count, h1: {count.le(a, b) == True{} : Bool}, h2: {count.le(c, d) == True{} : Bool}) -> Goal.count.add_mono(a, b, c, d):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Never{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Once{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Never{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Never{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h1)
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h1)
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h1)
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Once{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h1)
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h1)
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h1)
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Never{}, Laws.Repeated{}), h1)
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h1)
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h1)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h1)
                case Laws.Repeated{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h1)
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              match d:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match d:
                case Laws.Never{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h2)
                case Laws.Once{}:
                  false_ne_true(Goal.count.add_mono(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h2)
                case Laws.Repeated{}:
                  {==}

# le is transitive.
def Goal.count.le_trans(a: Laws.Count, b: Laws.Count, c: Laws.Count) -> Type:
  {count.le(a, c) == True{} : Bool}

def count.le_trans(a: Laws.Count, b: Laws.Count, c: Laws.Count, h1: {count.le(a, b) == True{} : Bool}, h2: {count.le(b, c) == True{} : Bool}) -> Goal.count.le_trans(a, b, c):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Never{}, Laws.Once{}, Laws.Never{}), h2)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h2)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h2)
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Never{}, Laws.Never{}), h1)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Never{}, Laws.Once{}), h1)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Never{}, Laws.Repeated{}), h1)
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Once{}, Laws.Never{}), h2)
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h2)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h2)
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Never{}, Laws.Never{}), h1)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Never{}, Laws.Once{}), h1)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), h1)
        case Laws.Once{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h1)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Once{}, Laws.Once{}), h1)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), h1)
        case Laws.Repeated{}:
          match c:
            case Laws.Never{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h2)
            case Laws.Once{}:
              false_ne_true(Goal.count.le_trans(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h2)
            case Laws.Repeated{}:
              {==}

# A Never sum has a Never left summand ...
def Goal.count.never_l(a: Laws.Count, b: Laws.Count) -> Type:
  {count.le(a, Laws.Never{}) == True{} : Bool}

def count.never_l(a: Laws.Count, b: Laws.Count, h: {count.le(Laws.count.add(a, b), Laws.Never{}) == True{} : Bool}) -> Goal.count.never_l(a, b):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.never_l(Laws.Never{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_l(Laws.Never{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_l(Laws.Once{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_l(Laws.Once{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_l(Laws.Once{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_l(Laws.Repeated{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_l(Laws.Repeated{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_l(Laws.Repeated{}, Laws.Repeated{}), h)

# ... and a Never right summand.
def Goal.count.never_r(a: Laws.Count, b: Laws.Count) -> Type:
  {count.le(b, Laws.Never{}) == True{} : Bool}

def count.never_r(a: Laws.Count, b: Laws.Count, h: {count.le(Laws.count.add(a, b), Laws.Never{}) == True{} : Bool}) -> Goal.count.never_r(a, b):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.never_r(Laws.Never{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_r(Laws.Never{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_r(Laws.Once{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_r(Laws.Once{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_r(Laws.Once{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_r(Laws.Repeated{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_r(Laws.Repeated{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_r(Laws.Repeated{}, Laws.Repeated{}), h)

# Two Never summands make a Never sum.
def Goal.count.never_add(a: Laws.Count, b: Laws.Count) -> Type:
  {count.le(Laws.count.add(a, b), Laws.Never{}) == True{} : Bool}

def count.never_add(a: Laws.Count, b: Laws.Count, ha: {count.le(a, Laws.Never{}) == True{} : Bool}, hb: {count.le(b, Laws.Never{}) == True{} : Bool}) -> Goal.count.never_add(a, b):
  match a:
    case Laws.Never{}:
      match b:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.never_add(Laws.Never{}, Laws.Once{}), hb)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_add(Laws.Never{}, Laws.Repeated{}), hb)
    case Laws.Once{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_add(Laws.Once{}, Laws.Never{}), ha)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_add(Laws.Once{}, Laws.Once{}), ha)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_add(Laws.Once{}, Laws.Repeated{}), ha)
    case Laws.Repeated{}:
      match b:
        case Laws.Never{}:
          false_ne_true(Goal.count.never_add(Laws.Repeated{}, Laws.Never{}), ha)
        case Laws.Once{}:
          false_ne_true(Goal.count.never_add(Laws.Repeated{}, Laws.Once{}), ha)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.never_add(Laws.Repeated{}, Laws.Repeated{}), ha)

# A Never sum has a False head.
def Goal.count.not_of(x: Bool, q: Laws.Count) -> Type:
  {Bool.not(x) == True{} : Bool}

def count.not_of(x: Bool, q: Laws.Count, h: {count.le(Laws.count.add(Laws.count.of(x), q), Laws.Never{}) == True{} : Bool}) -> Goal.count.not_of(x, q):
  match x:
    case False{}:
      match q:
        case Laws.Never{}:
          {==}
        case Laws.Once{}:
          false_ne_true(Goal.count.not_of(False{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.not_of(False{}, Laws.Repeated{}), h)
    case True{}:
      match q:
        case Laws.Never{}:
          false_ne_true(Goal.count.not_of(True{}, Laws.Never{}), h)
        case Laws.Once{}:
          false_ne_true(Goal.count.not_of(True{}, Laws.Once{}), h)
        case Laws.Repeated{}:
          false_ne_true(Goal.count.not_of(True{}, Laws.Repeated{}), h)

# Flushing moves a batch to the front of three summands.
def Goal.count.swap3_le(w: Laws.Count, r: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(w, Laws.count.add(r, i)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}

def count.swap3_le(w: Laws.Count, r: Laws.Count, i: Laws.Count) -> Goal.count.swap3_le(w, r, i):
  match w:
    case Laws.Never{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Once{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
    case Laws.Repeated{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              {==}
            case Laws.Repeated{}:
              {==}

# The same move keeps a Never sum Never.
def Goal.count.swap3_never(w: Laws.Count, r: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(w, Laws.count.add(r, i)), Laws.Never{}) == True{} : Bool}

def count.swap3_never(w: Laws.Count, r: Laws.Count, i: Laws.Count, h: {count.le(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{}) == True{} : Bool}) -> Goal.count.swap3_never(w, r, i):
  match w:
    case Laws.Never{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              {==}
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Never{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Once{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Never{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Once{}, Laws.Repeated{}, Laws.Repeated{}), h)
    case Laws.Repeated{}:
      match r:
        case Laws.Never{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Never{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Never{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Never{}, Laws.Repeated{}), h)
        case Laws.Once{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Once{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Once{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Once{}, Laws.Repeated{}), h)
        case Laws.Repeated{}:
          match i:
            case Laws.Never{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Repeated{}, Laws.Never{}), h)
            case Laws.Once{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Repeated{}, Laws.Once{}), h)
            case Laws.Repeated{}:
              false_ne_true(Goal.count.swap3_never(Laws.Repeated{}, Laws.Repeated{}, Laws.Repeated{}), h)

# An arrival queued in the read batch: one more entry, one arrival fewer.
def Goal.count.or_run_r(x: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.add(Laws.count.of(x), r), Laws.count.add(w, i)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.count.of(x))) == True{} : Bool}

def count.or_run_r(x: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Goal.count.or_run_r(x, r, w, i):
  match x:
    case False{}:
      match r:
        case Laws.Never{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Once{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
    case True{}:
      match r:
        case Laws.Never{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Once{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}

# The same, queued in the write batch.
def Goal.count.or_run_w(x: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(r, Laws.count.add(Laws.count.add(Laws.count.of(x), w), i)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.count.of(x))) == True{} : Bool}

def count.or_run_w(x: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Goal.count.or_run_w(x, r, w, i):
  match x:
    case False{}:
      match r:
        case Laws.Never{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Once{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
    case True{}:
      match r:
        case Laws.Never{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Once{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
        case Laws.Repeated{}:
          match w:
            case Laws.Never{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Once{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}
            case Laws.Repeated{}:
              match i:
                case Laws.Never{}:
                  {==}
                case Laws.Once{}:
                  {==}
                case Laws.Repeated{}:
                  {==}

# An arrival y queued in a read batch whose id is b (z) or is not:
# a Queue in b or an entry outside b, never both.
def Goal.count.fb_run_r(z: Bool, y: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.add(Laws.count.of(Bool.and(z, y)), Laws.Never{}), Laws.count.add(Laws.count.add(Laws.count.of(Bool.and(y, Bool.not(z))), r), Laws.count.add(w, i))), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.count.of(y))) == True{} : Bool}

def count.fb_run_r(z: Bool, y: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Goal.count.fb_run_r(z, y, r, w, i):
  match z:
    case False{}:
      match y:
        case False{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
        case True{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
    case True{}:
      match y:
        case False{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
        case True{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}

# The same, queued in a write batch.
def Goal.count.fb_run_w(z: Bool, y: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Type:
  {count.le(Laws.count.add(Laws.count.add(Laws.count.of(Bool.and(z, y)), Laws.Never{}), Laws.count.add(r, Laws.count.add(Laws.count.add(Laws.count.of(Bool.and(y, Bool.not(z))), w), i))), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.count.of(y))) == True{} : Bool}

def count.fb_run_w(z: Bool, y: Bool, r: Laws.Count, w: Laws.Count, i: Laws.Count) -> Goal.count.fb_run_w(z, y, r, w, i):
  match z:
    case False{}:
      match y:
        case False{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
        case True{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
    case True{}:
      match y:
        case False{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
        case True{}:
          match r:
            case Laws.Never{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Once{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
            case Laws.Repeated{}:
              match w:
                case Laws.Never{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Once{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}
                case Laws.Repeated{}:
                  match i:
                    case Laws.Never{}:
                      {==}
                    case Laws.Once{}:
                      {==}
                    case Laws.Repeated{}:
                      {==}

# A pushed entry whose flag is False{} adds nothing to a Never sum.
def count.push_none_r(x: Bool, -r: Laws.Count, -w: Laws.Count, -i: Laws.Count, hx: {x == False{} : Bool}, hp: {count.le(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{}) == True{} : Bool}) -> {count.le(Laws.count.add(Laws.count.add(Laws.count.of(x), r), Laws.count.add(w, i)), Laws.Never{}) == True{} : Bool}:
  match x:
    case False{}:
      hp
    case True{}:
      true_ne_false({count.le(Laws.count.add(Laws.count.add(Laws.count.of(True{}), r), Laws.count.add(w, i)), Laws.Never{}) == True{} : Bool}, hx)

def count.push_none_w(x: Bool, -r: Laws.Count, -w: Laws.Count, -i: Laws.Count, hx: {x == False{} : Bool}, hp: {count.le(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{}) == True{} : Bool}) -> {count.le(Laws.count.add(r, Laws.count.add(Laws.count.add(Laws.count.of(x), w), i)), Laws.Never{}) == True{} : Bool}:
  match x:
    case False{}:
      hp
    case True{}:
      true_ne_false({count.le(Laws.count.add(r, Laws.count.add(Laws.count.add(Laws.count.of(True{}), w), i)), Laws.Never{}) == True{} : Bool}, hx)

# A Never head drops out of a bound.
def count.fb_q(-q: Laws.Count, -x: Laws.Count, -t: Laws.Count, hq: {q == Laws.Never{} : Laws.Count}, h: {count.le(x, t) == True{} : Bool}) -> {count.le(Laws.count.add(q, x), t) == True{} : Bool}:
  %Equal.sym(Laws.Count, q, Laws.Never{}, hq) : {count.le(Laws.count.add(_, x), t) == True{} : Bool}
  h

# Settling a batch in each of the four ways the open batches can move:
# replies a and kept entries k together equal the entries before (e).
def count.or_tt(+a: Laws.Count, +k: Laws.Count, +r: Laws.Count, +w: Laws.Count, +i: Laws.Count, e: {Laws.count.add(a, k) == Laws.count.add(w, Laws.count.add(r, i)) : Laws.Count}) -> {count.le(Laws.count.add(a, k), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}:
  %Equal.sym(Laws.Count, Laws.count.add(a, k), Laws.count.add(w, Laws.count.add(r, i)), e) : {count.le(_, Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  count.swap3_le(w, r, i)

def count.or_tf(+a: Laws.Count, +k: Laws.Count, +r: Laws.Count, +w: Laws.Count, +i: Laws.Count, e: {Laws.count.add(a, k) == Laws.count.add(r, i) : Laws.Count}) -> {count.le(Laws.count.add(a, Laws.count.add(w, k)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}:
  %Equal.sym(Laws.Count, Laws.count.add(a, Laws.count.add(w, k)), Laws.count.add(w, Laws.count.add(a, k)), count.swap(a, w, k)) : {count.le(_, Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(a, k), Laws.count.add(r, i), e) : {count.le(Laws.count.add(w, _), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  count.swap3_le(w, r, i)

def count.or_ft(+a: Laws.Count, +k: Laws.Count, +r: Laws.Count, +w: Laws.Count, +i: Laws.Count, e: {Laws.count.add(a, k) == Laws.count.add(w, i) : Laws.Count}) -> {count.le(Laws.count.add(a, Laws.count.add(r, k)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}:
  %Equal.sym(Laws.Count, Laws.count.add(a, Laws.count.add(r, k)), Laws.count.add(r, Laws.count.add(a, k)), count.swap(a, r, k)) : {count.le(_, Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(a, k), Laws.count.add(w, i), e) : {count.le(Laws.count.add(r, _), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  count.same(Laws.count.add(r, Laws.count.add(w, i)))

def count.or_ff(+a: Laws.Count, +k: Laws.Count, +r: Laws.Count, +w: Laws.Count, +i: Laws.Count, e: {Laws.count.add(a, k) == i : Laws.Count}) -> {count.le(Laws.count.add(a, Laws.count.add(r, Laws.count.add(w, k))), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}:
  %Equal.sym(Laws.Count, Laws.count.add(a, Laws.count.add(r, Laws.count.add(w, k))), Laws.count.add(r, Laws.count.add(a, Laws.count.add(w, k))), count.swap(a, r, Laws.count.add(w, k))) : {count.le(_, Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(a, Laws.count.add(w, k)), Laws.count.add(w, Laws.count.add(a, k)), count.swap(a, w, k)) : {count.le(Laws.count.add(r, _), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(a, k), i, e) : {count.le(Laws.count.add(r, Laws.count.add(w, _)), Laws.count.add(Laws.count.add(r, Laws.count.add(w, i)), Laws.Never{})) == True{} : Bool}
  count.same(Laws.count.add(r, Laws.count.add(w, i)))

# reply_to_the_same_generation
# ----------------------------
# Invariant, for the state (s, c) reached after a prefix and the commands acc
# emitted before c: every Send in acc ++ c names a (req, conn, gen) that
# arrived in the whole trace hist, and so does every entry of the two open
# batches and of every batch in flight. Arrived adds an entry or a Send for
# the event itself, which is in hist; flushing moves a batch into inflight
# and opens an empty one; Synced and Died turn the entries of the batches
# with their id into Sends; keep and drop only remove.

def reply_to_the_same_generation.pa(+hist: List<&2, Pol.HostEvent>, es: List<&2, Pol.Pending>) -> Bool:
  match es:
    case Nil{}:
      True{}
    case p <> rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, op}:
          Bool.and(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, rest))

def reply_to_the_same_generation.pa_bt(+hist: List<&2, Pol.HostEvent>, bt: Pol.Batch) -> Bool:
  match bt:
    case Pol.Batch{id, kind, entries}:
      reply_to_the_same_generation.pa(hist, entries)

def reply_to_the_same_generation.pa_bs(+hist: List<&2, Pol.HostEvent>, bs: List<&2, Pol.Batch>) -> Bool:
  match bs:
    case Nil{}:
      True{}
    case bt <> rest:
      Bool.and(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest))

def reply_to_the_same_generation.Inv(+hist: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>) -> Type:
  match t:
    case (s, c):
      match s:
        case Pol.State{next, reads, writes, inflight}:
          {Laws.sends_match_arrivals(hist, List.append(&2, Pol.Command, acc, c)) == True{} : Bool} & {reply_to_the_same_generation.pa_bt(hist, reads) == True{} : Bool} & {reply_to_the_same_generation.pa_bt(hist, writes) == True{} : Bool} & {reply_to_the_same_generation.pa_bs(hist, inflight) == True{} : Bool}

def reply_to_the_same_generation.matches_self(+req: U32, +conn: U32, +gen: U32, -r: Req.Request) -> {Laws.arrival_matches(req, conn, gen, Pol.Arrived{req, conn, gen, r}) == True{} : Bool}:
  %Equal.sym(Bool, U32.is_eq(req, req), True{}, u32_eq_refl(req)) : {Bool.and(_, Bool.and(U32.is_eq(conn, conn), U32.is_eq(gen, gen))) == True{} : Bool}
  %Equal.sym(Bool, U32.is_eq(conn, conn), True{}, u32_eq_refl(conn)) : {Bool.and(_, U32.is_eq(gen, gen)) == True{} : Bool}
  %Equal.sym(Bool, U32.is_eq(gen, gen), True{}, u32_eq_refl(gen)) : {_ == True{} : Bool}
  {==}

def reply_to_the_same_generation.found(+req: U32, +conn: U32, +gen: U32, -r: Req.Request, hist: List<&2, Pol.HostEvent>, m: Mem(Pol.HostEvent, Pol.Arrived{req, conn, gen, r}, hist)) -> {Laws.arrived(req, conn, gen, hist) == True{} : Bool}:
  match hist:
    case Nil{}:
      match m:
    case h <> t:
      match m:
        case Inl{e}:
          %Equal.sym(Pol.HostEvent, h, Pol.Arrived{req, conn, gen, r}, e) : {Bool.or(Laws.arrival_matches(req, conn, gen, _), Laws.arrived(req, conn, gen, t)) == True{} : Bool}
          %Equal.sym(Bool, Laws.arrival_matches(req, conn, gen, Pol.Arrived{req, conn, gen, r}), True{}, reply_to_the_same_generation.matches_self(req, conn, gen, r)) : {Bool.or(_, Laws.arrived(req, conn, gen, t)) == True{} : Bool}
          {==}
        case Inr{n}:
          %Equal.sym(Bool, Laws.arrived(req, conn, gen, t), True{}, reply_to_the_same_generation.found(req, conn, gen, r, t, n)) : {Bool.or(Laws.arrival_matches(req, conn, gen, h), _) == True{} : Bool}
          or_true_r(Laws.arrival_matches(req, conn, gen, h))

def reply_to_the_same_generation.sma_append(+hist: List<&2, Pol.HostEvent>, +xs: List<&2, Pol.Command>, +ys: List<&2, Pol.Command>, +hx: {Laws.sends_match_arrivals(hist, xs) == True{} : Bool}, hy: {Laws.sends_match_arrivals(hist, ys) == True{} : Bool}) -> {Laws.sends_match_arrivals(hist, List.append(&2, Pol.Command, xs, ys)) == True{} : Bool}:
  match xs:
    case Nil{}:
      hy
    case x <> rest:
      match x:
        case Pol.Send{+req, +conn, +gen, reply}:
          and_intro(Laws.arrived(req, conn, gen, hist), Laws.sends_match_arrivals(hist, List.append(&2, Pol.Command, rest, ys)), and_l(Laws.arrived(req, conn, gen, hist), Laws.sends_match_arrivals(hist, rest), hx), reply_to_the_same_generation.sma_append(hist, rest, ys, and_r(Laws.arrived(req, conn, gen, hist), Laws.sends_match_arrivals(hist, rest), hx), hy))
        case Pol.Queue{b, req, op}:
          reply_to_the_same_generation.sma_append(hist, rest, ys, hx, hy)
        case Pol.Open{b, k}:
          reply_to_the_same_generation.sma_append(hist, rest, ys, hx, hy)

# Arrived.

def reply_to_the_same_generation.push(+hist: List<&2, Pol.HostEvent>, +req: U32, +conn: U32, +gen: U32, -op: Req.Op, -r: Req.Request, bt: Pol.Batch, m: Mem(Pol.HostEvent, Pol.Arrived{req, conn, gen, r}, hist), h: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bt(hist, Pol.push(Pol.Pending{req, conn, gen, op}, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      and_intro(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, entries), reply_to_the_same_generation.found(req, conn, gen, r, hist, m), h)

def reply_to_the_same_generation.put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, -r: Req.Request, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, m: Mem(Pol.HostEvent, Pol.Arrived{req, conn, gen, r}, hist), h1: {Laws.sends_match_arrivals(hist, x) == True{} : Bool}, h2: {reply_to_the_same_generation.pa_bt(hist, reads) == True{} : Bool}, h3: {reply_to_the_same_generation.pa_bt(hist, writes) == True{} : Bool}, h4: {reply_to_the_same_generation.pa_bs(hist, inflight) == True{} : Bool}) -> reply_to_the_same_generation.Inv(hist, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x):
  match k:
    case Pol.Read{}:
      (reply_to_the_same_generation.sma_append(hist, x, [Pol.Queue{Pol.id_of(reads), req, op}], h1, {==}), reply_to_the_same_generation.push(hist, req, conn, gen, op, r, reads, m, h2), h3, h4)
    case Pol.Write{}:
      (reply_to_the_same_generation.sma_append(hist, x, [Pol.Queue{Pol.id_of(writes), req, op}], h1, {==}), h2, reply_to_the_same_generation.push(hist, req, conn, gen, op, r, writes, m, h3), h4)

def reply_to_the_same_generation.arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, -r: Req.Request, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, m: Mem(Pol.HostEvent, Pol.Arrived{req, conn, gen, r}, hist), h1: {Laws.sends_match_arrivals(hist, x) == True{} : Bool}, h2: {reply_to_the_same_generation.pa_bt(hist, reads) == True{} : Bool}, h3: {reply_to_the_same_generation.pa_bt(hist, writes) == True{} : Bool}, h4: {reply_to_the_same_generation.pa_bs(hist, inflight) == True{} : Bool}) -> reply_to_the_same_generation.Inv(hist, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x):
  match d:
    case Req.Reject{status, msg}:
      (reply_to_the_same_generation.sma_append(hist, x, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}], h1, and_intro(Laws.arrived(req, conn, gen, hist), True{}, reply_to_the_same_generation.found(req, conn, gen, r, hist, m), {==})), h2, h3, h4)
    case Req.Run{+op}:
      reply_to_the_same_generation.put(Pol.kind_of(op), req, conn, gen, op, r, next, reads, writes, inflight, x, hist, m, h1, h2, h3, h4)

# Flushed, Synced and Died.

def reply_to_the_same_generation.opened(h: Bool, -n: U32, -k: Pol.BatchKind, -hist: List<&2, Pol.HostEvent>, -rest: List<&2, Pol.Command>, ih: {Laws.sends_match_arrivals(hist, rest) == True{} : Bool}) -> {Laws.sends_match_arrivals(hist, Pol.opened(h, n, k, rest)) == True{} : Bool}:
  match h:
    case True{}:
      ih
    case False{}:
      ih

def reply_to_the_same_generation.fresh(h: Bool, -n: U32, -k: Pol.BatchKind, -hist: List<&2, Pol.HostEvent>, -bt: Pol.Batch, hb: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bt(hist, Pol.fresh(h, n, k, bt)) == True{} : Bool}:
  match h:
    case True{}:
      {==}
    case False{}:
      hb

def reply_to_the_same_generation.park(h: Bool, -hist: List<&2, Pol.HostEvent>, -bt: Pol.Batch, -bs: List<&2, Pol.Batch>, hb: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}, hs: {reply_to_the_same_generation.pa_bs(hist, bs) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bs(hist, Pol.park(h, bt, bs)) == True{} : Bool}:
  match h:
    case True{}:
      and_intro(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, bs), hb, hs)
    case False{}:
      hs

def reply_to_the_same_generation.answer(+ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, +h: {reply_to_the_same_generation.pa(hist, es) == True{} : Bool}, ih: {Laws.sends_match_arrivals(hist, rest) == True{} : Bool}) -> {Laws.sends_match_arrivals(hist, Pol.answer(ok, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, +conn, +gen, op}:
          and_intro(Laws.arrived(req, conn, gen, hist), Laws.sends_match_arrivals(hist, Pol.answer(ok, all, u, tail, Pol.after(left, req), rest)), and_l(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, tail), h), reply_to_the_same_generation.answer(ok, all, u, hist, tail, Pol.after(left, req), rest, and_r(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, tail), h), ih))

def reply_to_the_same_generation.answer_in.put(hit: Bool, bt: Pol.Batch, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, -rest: List<&2, Pol.Command>, hb: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}, ih: {Laws.sends_match_arrivals(hist, rest) == True{} : Bool}) -> {Laws.sends_match_arrivals(hist, Pol.answer_in.put(hit, bt, ok, outs, u, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          reply_to_the_same_generation.answer(ok, outs, u, hist, entries, outs, rest, hb, ih)
    case False{}:
      ih

def reply_to_the_same_generation.answer_in(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, bs: List<&2, Pol.Batch>, +h: {reply_to_the_same_generation.pa_bs(hist, bs) == True{} : Bool}) -> {Laws.sends_match_arrivals(hist, Pol.answer_in(b, ok, outs, u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      reply_to_the_same_generation.answer_in.put(U32.is_eq(Pol.id_of(bt), b), bt, ok, outs, u, hist, Pol.answer_in(b, ok, outs, u, rest), and_l(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h), reply_to_the_same_generation.answer_in(b, ok, outs, u, hist, rest, and_r(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h)))

def reply_to_the_same_generation.keep.put(hit: Bool, -bt: Pol.Batch, -hist: List<&2, Pol.HostEvent>, -rest: List<&2, Pol.Batch>, hb: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}, ih: {reply_to_the_same_generation.pa_bs(hist, rest) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bs(hist, Pol.keep.put(hit, bt, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      ih
    case False{}:
      and_intro(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), hb, ih)

def reply_to_the_same_generation.keep(+b: U32, +hist: List<&2, Pol.HostEvent>, bs: List<&2, Pol.Batch>, +h: {reply_to_the_same_generation.pa_bs(hist, bs) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bs(hist, Pol.keep(b, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      reply_to_the_same_generation.keep.put(U32.is_eq(Pol.id_of(bt), b), bt, hist, Pol.keep(b, rest), and_l(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h), reply_to_the_same_generation.keep(b, hist, rest, and_r(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h)))

def reply_to_the_same_generation.flush(+hr: Bool, +hw: Bool, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, h1: {Laws.sends_match_arrivals(hist, x) == True{} : Bool}, +h2: {reply_to_the_same_generation.pa_bt(hist, reads) == True{} : Bool}, +h3: {reply_to_the_same_generation.pa_bt(hist, writes) == True{} : Bool}, +h4: {reply_to_the_same_generation.pa_bs(hist, inflight) == True{} : Bool}) -> reply_to_the_same_generation.Inv(hist, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), x):
  (reply_to_the_same_generation.sma_append(hist, x, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{})), h1, reply_to_the_same_generation.opened(hr, next, Pol.Read{}, hist, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}), reply_to_the_same_generation.opened(hw, Pol.bump(hr, next), Pol.Write{}, hist, Nil{}, {==}))), reply_to_the_same_generation.fresh(hr, next, Pol.Read{}, hist, reads, h2), reply_to_the_same_generation.fresh(hw, Pol.bump(hr, next), Pol.Write{}, hist, writes, h3), reply_to_the_same_generation.park(hw, hist, writes, Pol.park(hr, reads, inflight), h3, reply_to_the_same_generation.park(hr, hist, reads, inflight, h2, h4)))

def reply_to_the_same_generation.settle(+hr: Bool, +hw: Bool, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, h1: {Laws.sends_match_arrivals(hist, x) == True{} : Bool}, +h2: {reply_to_the_same_generation.pa_bt(hist, reads) == True{} : Bool}, +h3: {reply_to_the_same_generation.pa_bt(hist, writes) == True{} : Bool}, +h4: {reply_to_the_same_generation.pa_bs(hist, inflight) == True{} : Bool}) -> reply_to_the_same_generation.Inv(hist, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), x):
  (reply_to_the_same_generation.sma_append(hist, x, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))))), h1, reply_to_the_same_generation.opened(hr, next, Pol.Read{}, hist, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), reply_to_the_same_generation.opened(hw, Pol.bump(hr, next), Pol.Write{}, hist, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))), reply_to_the_same_generation.answer_in(b, ok, outs, u, hist, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), reply_to_the_same_generation.park(hw, hist, writes, Pol.park(hr, reads, inflight), h3, reply_to_the_same_generation.park(hr, hist, reads, inflight, h2, h4)))))), reply_to_the_same_generation.fresh(hr, next, Pol.Read{}, hist, reads, h2), reply_to_the_same_generation.fresh(hw, Pol.bump(hr, next), Pol.Write{}, hist, writes, h3), reply_to_the_same_generation.keep(b, hist, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), reply_to_the_same_generation.park(hw, hist, writes, Pol.park(hr, reads, inflight), h3, reply_to_the_same_generation.park(hr, hist, reads, inflight, h2, h4))))

# Gone.

def reply_to_the_same_generation.drop.put(hit: Bool, -req: U32, -conn: U32, -gen: U32, -op: Req.Op, -hist: List<&2, Pol.HostEvent>, -rest: List<&2, Pol.Pending>, ha: {Laws.arrived(req, conn, gen, hist) == True{} : Bool}, ih: {reply_to_the_same_generation.pa(hist, rest) == True{} : Bool}) -> {reply_to_the_same_generation.pa(hist, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      ih
    case False{}:
      and_intro(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, rest), ha, ih)

def reply_to_the_same_generation.drop(+cg: U32, +gg: U32, +hist: List<&2, Pol.HostEvent>, es: List<&2, Pol.Pending>, +h: {reply_to_the_same_generation.pa(hist, es) == True{} : Bool}) -> {reply_to_the_same_generation.pa(hist, Pol.drop(cg, gg, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          reply_to_the_same_generation.drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), req, conn, gen, op, hist, Pol.drop(cg, gg, rest), and_l(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, rest), h), reply_to_the_same_generation.drop(cg, gg, hist, rest, and_r(Laws.arrived(req, conn, gen, hist), reply_to_the_same_generation.pa(hist, rest), h)))

def reply_to_the_same_generation.drop_bt(+cg: U32, +gg: U32, +hist: List<&2, Pol.HostEvent>, bt: Pol.Batch, h: {reply_to_the_same_generation.pa_bt(hist, bt) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bt(hist, Pol.drop_batch(cg, gg, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      reply_to_the_same_generation.drop(cg, gg, hist, entries, h)

def reply_to_the_same_generation.drop_all(+cg: U32, +gg: U32, +hist: List<&2, Pol.HostEvent>, bs: List<&2, Pol.Batch>, +h: {reply_to_the_same_generation.pa_bs(hist, bs) == True{} : Bool}) -> {reply_to_the_same_generation.pa_bs(hist, Pol.drop_all(cg, gg, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      and_intro(reply_to_the_same_generation.pa_bt(hist, Pol.drop_batch(cg, gg, bt)), reply_to_the_same_generation.pa_bs(hist, Pol.drop_all(cg, gg, rest)), reply_to_the_same_generation.drop_bt(cg, gg, hist, bt, and_l(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h)), reply_to_the_same_generation.drop_all(cg, gg, hist, rest, and_r(reply_to_the_same_generation.pa_bt(hist, bt), reply_to_the_same_generation.pa_bs(hist, rest), h)))

def reply_to_the_same_generation.step(+s: Pol.State, +e: Pol.HostEvent, +c: List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, m: Mem(Pol.HostEvent, e, hist), hinv: reply_to_the_same_generation.Inv(hist, (s, c), acc)) -> reply_to_the_same_generation.Inv(hist, Pol.step(s, e), List.append(&2, Pol.Command, acc, c)):
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, r}:
          match hinv:
            case (h1, h2, h3, h4):
              reply_to_the_same_generation.arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, r, List.append(&2, Pol.Command, acc, c), hist, m, h1, h2, h3, h4)
        case Pol.Flushed{+b}:
          match hinv:
            case (h1, h2, h3, h4):
              reply_to_the_same_generation.flush(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, h1, h2, h3, h4)
        case Pol.Synced{+b, +ok, +outs, +u}:
          match hinv:
            case (h1, h2, h3, h4):
              reply_to_the_same_generation.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, ok, outs, u, next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, h1, h2, h3, h4)
        case Pol.Died{+b}:
          match hinv:
            case (h1, h2, h3, h4):
              reply_to_the_same_generation.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, False{}, Nil{}, 0, next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, h1, h2, h3, h4)
        case Pol.Gone{+cg, +gg}:
          match hinv:
            case (h1, h2, h3, h4):
              (reply_to_the_same_generation.sma_append(hist, List.append(&2, Pol.Command, acc, c), Nil{}, h1, {==}), reply_to_the_same_generation.drop_bt(cg, gg, hist, reads, h2), reply_to_the_same_generation.drop_bt(cg, gg, hist, writes, h3), reply_to_the_same_generation.drop_all(cg, gg, hist, inflight, h4))

def reply_to_the_same_generation.go(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, hes: AllIn(Pol.HostEvent, es, hist), hinv: reply_to_the_same_generation.Inv(hist, t, acc)) -> {Laws.sends_match_arrivals(hist, Pol.commands(Pol.run.go(es, t, acc))) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (s, c):
          match s:
            case Pol.State{next, reads, writes, inflight}:
              match hinv:
                case (h1, h2, h3, h4):
                  h1
    case e <> rest:
      match t:
        case (+s, +c):
          match hes:
            case (m, hrest):
              reply_to_the_same_generation.go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), hist, hrest, reply_to_the_same_generation.step(s, e, c, acc, hist, m, hinv))

def Laws.reply_to_the_same_generation(es):
  reply_to_the_same_generation.go(es, (Pol.state0(), Nil{}), Nil{}, es, all_in_self(Pol.HostEvent, es), ({==}, {==}, {==}, {==}))

# unknown_is_not_success
# ----------------------
# step on Synced{b, ok, outs, u} emits an Open for each open batch with id
# b, then answer_in(b, ok, outs, u, bs): a reply for each entry of each
# batch in bs with id b. Each reply is respond(op, outcome(ok, outs, left,
# req), u), where left is what the entries before it have left of outs.
# If ok is False{} that outcome is Unknown{}. If ok is True{} it is
# head(left, outs, req): the outcome of left's first pair when that pair
# has the id req, else find(outs, req). By induction on outs, find returns
# Unknown{} whenever has_outcome is False{}; the pair at the head of left
# is a pair of outs (left is a part of outs: AllIn, kept by after), so
# when its id is req, has_outcome is True{}. no_success_on_unknown (S02)
# closes the Unknown{} cases.

def unknown_is_not_success.pick(hit: Bool, -o: Rsp.Outcome, -op: Req.Op, -u: U32, -x: Bool, -f: Rsp.Outcome, ih: {Laws.implies(Bool.not(x), Bool.not(Rsp.is_success(Rsp.respond(op, f, u)))) == True{} : Bool}) -> {Laws.implies(Bool.not(Bool.or(hit, x)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.find.pick(hit, o, f), u)))) == True{} : Bool}:
  match hit:
    case True{}:
      {==}
    case False{}:
      ih

def unknown_is_not_success.find(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, +u: U32) -> {Laws.implies(Bool.not(Laws.has_outcome(outs, req)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.find(outs, req), u)))) == True{} : Bool}:
  match outs:
    case Nil{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(op, u)) : {Laws.implies(True{}, Bool.not(_)) == True{} : Bool}
      {==}
    case (r, o) <> +rest:
      unknown_is_not_success.pick(U32.is_eq(r, req), o, op, u, Laws.has_outcome(rest, req), Pol.find(rest, req), unknown_is_not_success.find(rest, req, op, u))

def unknown_is_not_success.mem_has(all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -r: U32, -o: Rsp.Outcome, +req: U32, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), heq: {U32.is_eq(r, req) == True{} : Bool}) -> {Laws.has_outcome(all, req) == True{} : Bool}:
  match all:
    case Nil{}:
      match m:
    case (+r2, o2) <> +t:
      match m:
        case Inl{e}:
          %Equal.sym(+Sigma<U32, _ => Rsp.Outcome>, (r2, o2), (r, o), e) : {Laws.has_outcome(_ <> t, req) == True{} : Bool}
          %Equal.sym(Bool, U32.is_eq(r, req), True{}, heq) : {Bool.or(_, Laws.has_outcome(t, req)) == True{} : Bool}
          {==}
        case Inr{n}:
          %Equal.sym(Bool, Laws.has_outcome(t, req), True{}, unknown_is_not_success.mem_has(t, r, o, req, n, heq)) : {Bool.or(U32.is_eq(r2, req), _) == True{} : Bool}
          or_true_r(U32.is_eq(r2, req))

def unknown_is_not_success.head.pick(hit: Bool, -r: U32, -o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, +u: U32, heq: {U32.is_eq(r, req) == hit : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all)) -> {Laws.implies(Bool.not(Laws.has_outcome(all, req)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.head.pick(hit, o, all, req), u)))) == True{} : Bool}:
  match hit:
    case True{}:
      %Equal.sym(Bool, Laws.has_outcome(all, req), True{}, unknown_is_not_success.mem_has(all, r, o, req, m, heq)) : {Laws.implies(Bool.not(_), Bool.not(Rsp.is_success(Rsp.respond(op, o, u)))) == True{} : Bool}
      {==}
    case False{}:
      unknown_is_not_success.find(all, req, op, u)

def unknown_is_not_success.head(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, +u: U32, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(Bool.not(Laws.has_outcome(all, req)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.head(left, all, req), u)))) == True{} : Bool}:
  match left:
    case Nil{}:
      unknown_is_not_success.find(all, req, op, u)
    case (+r, o) <> rest:
      match hl:
        case (m, hr):
          unknown_is_not_success.head.pick(U32.is_eq(r, req), r, o, all, req, op, u, {==}, m)

def unknown_is_not_success.reply(ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, +u: U32, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(Bool.not(Laws.has_outcome(all, req)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), u)))) == True{} : Bool}:
  match ok:
    case True{}:
      unknown_is_not_success.head(left, all, req, op, u, hl)
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(op, u)) : {Laws.implies(Bool.not(Laws.has_outcome(all, req)), Bool.not(_)) == True{} : Bool}
      implies_true_r(Bool.not(Laws.has_outcome(all, req)))

def unknown_is_not_success.skip.pick(hit: Bool, -rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -later: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all), hs: AllIn(+Sigma<U32, _ => Rsp.Outcome>, later, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip.pick(hit, rest, later), all):
  match hit:
    case True{}:
      hr
    case False{}:
      hs

def unknown_is_not_success.skip(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, +hr):
          unknown_is_not_success.skip.pick(U32.is_eq(r, req), rest, Pol.skip(rest, req), all, hr, unknown_is_not_success.skip(rest, req, all, hr))

def unknown_is_not_success.after.pick(hit: Bool, rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after.pick(hit, rest, req), all):
  match hit:
    case True{}:
      hr
    case False{}:
      unknown_is_not_success.skip(rest, req, all, hr)

def unknown_is_not_success.after(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, hr):
          unknown_is_not_success.after.pick(U32.is_eq(r, req), rest, req, all, hr)

def unknown_is_not_success.answer(+ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, +hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all), ih: {Laws.missing_fails(all, rest) == True{} : Bool}) -> {Laws.missing_fails(all, Pol.answer(ok, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, +op}:
          and_intro(Laws.implies(Bool.not(Laws.has_outcome(all, req)), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), u)))), Laws.missing_fails(all, Pol.answer(ok, all, u, tail, Pol.after(left, req), rest)), unknown_is_not_success.reply(ok, all, left, req, op, u, hl), unknown_is_not_success.answer(ok, all, u, tail, Pol.after(left, req), rest, unknown_is_not_success.after(left, req, all, hl), ih))

def unknown_is_not_success.answer_in.put(hit: Bool, bt: Pol.Batch, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, -rest: List<&2, Pol.Command>, ih: {Laws.missing_fails(outs, rest) == True{} : Bool}) -> {Laws.missing_fails(outs, Pol.answer_in.put(hit, bt, ok, outs, u, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          unknown_is_not_success.answer(ok, outs, u, entries, outs, rest, all_in_self(+Sigma<U32, _ => Rsp.Outcome>, outs), ih)
    case False{}:
      ih

def unknown_is_not_success.answer_in(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, bs: List<&2, Pol.Batch>) -> {Laws.missing_fails(outs, Pol.answer_in(b, ok, outs, u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      unknown_is_not_success.answer_in.put(U32.is_eq(Pol.id_of(bt), b), bt, ok, outs, u, Pol.answer_in(b, ok, outs, u, rest), unknown_is_not_success.answer_in(b, ok, outs, u, rest))

def unknown_is_not_success.opened(h: Bool, -n: U32, -k: Pol.BatchKind, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, ih: {Laws.missing_fails(outs, rest) == True{} : Bool}) -> {Laws.missing_fails(outs, Pol.opened(h, n, k, rest)) == True{} : Bool}:
  match h:
    case True{}:
      ih
    case False{}:
      ih

def unknown_is_not_success.settle(+hr: Bool, +hw: Bool, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {Laws.missing_fails(outs, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))) == True{} : Bool}:
  unknown_is_not_success.opened(hr, next, Pol.Read{}, outs, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), unknown_is_not_success.opened(hw, Pol.bump(hr, next), Pol.Write{}, outs, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))), unknown_is_not_success.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))

def unknown_is_not_success.settle_at(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {Laws.missing_fails(outs, Pol.commands(Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Synced{b, ok, outs, u}))) == True{} : Bool}:
  unknown_is_not_success.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, ok, outs, u, next, reads, writes, inflight)

def Laws.unknown_is_not_success(s, b, ok, outs, u):
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      unknown_is_not_success.settle_at(b, ok, outs, u, next, reads, writes, inflight)

# reject_commands_nothing
# -----------------------
# step on Arrived{req, conn, gen, r} emits arrived.commands(classify(r), ..).
# The lemma takes the decision d as a parameter and splits it: a Reject
# emits exactly [Send{..}], and the hypothesis is False{} == True{} for a Run.

def reject_commands_nothing.Goal(+d: Req.Decision, +reads: Pol.Batch, +writes: Pol.Batch, +req: U32, +conn: U32, +gen: U32) -> Type:
  {Laws.send_count(Pol.arrived.commands(d, reads, writes, req, conn, gen)) == Laws.Once{} : Laws.Count} & {Laws.queue_count(Pol.arrived.commands(d, reads, writes, req, conn, gen)) == Laws.Never{} : Laws.Count}

def reject_commands_nothing.arrived(d: Req.Decision, +reads: Pol.Batch, +writes: Pol.Batch, +req: U32, +conn: U32, +gen: U32, h: {Laws.is_reject(d) == True{} : Bool}) -> reject_commands_nothing.Goal(d, reads, writes, req, conn, gen):
  match d:
    case Req.Reject{status, msg}:
      ({==}, {==})
    case Req.Run{op}:
      false_ne_true(reject_commands_nothing.Goal(Req.Run{op}, reads, writes, req, conn, gen), h)

def Laws.reject_commands_nothing(s, req, conn, gen, r, h):
  match s:
    case Pol.State{next, reads, writes, inflight}:
      reject_commands_nothing.arrived(Req.classify(r), reads, writes, req, conn, gen, h)

# success_after_commit
# --------------------
# Invariant, for the state (s, c) after a prefix, the commands acc before c,
# the whole trace hist and the whole command list cs: every success Send in
# acc ++ c is backed in cs and hist, and every entry of a batch with id i
# (open or in flight) has its Queue{i, req, op} in cs. A reject's Send is
# never a success (classify rejects only with 400, 401 and 404). A Synced
# answers an entry of a batch with its id with a success only when ok is
# True{} and outs reports the commit of the entry's write (the S02 laws
# created_needs_commit, liked_needs_commit, no_success_on_unknown); the
# entry's Queue is in cs by the invariant and the Synced is an event of
# hist. A Died answers Unknown{}. cs is always the term
# commands(run.go(rest, t, acc)) itself, so it is the same list at every
# step without an equation to carry.

def success_after_commit.CS(es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>) -> List<&2, Pol.Command>:
  Pol.commands(Pol.run.go(es, t, acc))

def success_after_commit.reject_fails(d: Req.Decision) -> Bool:
  match d:
    case Req.Reject{status, msg}:
      Bool.not(Rsp.is_success(Rsp.Error{status, msg}))
    case Req.Run{op}:
      True{}

def success_after_commit.classify_fails(r: Req.Request) -> {success_after_commit.reject_fails(Req.classify(r)) == True{} : Bool}:
  match r:
    case Req.Request{route, auth, body}:
      match route:
        case Req.Health{}:
          {==}
        case Req.Feed{}:
          {==}
        case Req.PostById{pid}:
          match pid:
            case Req.ValidId{id}:
              {==}
            case Req.InvalidId{}:
              {==}
        case Req.CreatePost{}:
          match auth:
            case Req.User{uid, name}:
              match body:
                case Req.Trimmed{s}:
                  {==}
                case Req.TooLong{}:
                  {==}
                case Req.Missing{}:
                  {==}
                case Req.NoneSent{}:
                  {==}
                case Req.NotJson{}:
                  {==}
            case Req.BadPayload{}:
              {==}
            case Req.BadToken{}:
              {==}
            case Req.NoBearer{}:
              {==}
        case Req.LikePost{pid}:
          match pid:
            case Req.ValidId{id}:
              match auth:
                case Req.User{uid, name}:
                  {==}
                case Req.BadPayload{}:
                  {==}
                case Req.BadToken{}:
                  {==}
                case Req.NoBearer{}:
                  {==}
            case Req.InvalidId{}:
              match auth:
                case Req.User{uid, name}:
                  {==}
                case Req.BadPayload{}:
                  {==}
                case Req.BadToken{}:
                  {==}
                case Req.NoBearer{}:
                  {==}
        case Req.Unknown{}:
          {==}

def success_after_commit.backed_found(+s: Bool, +req: U32, +hist: List<&2, Pol.HostEvent>, cs: List<&2, Pol.Command>, -x: Pol.Command, m: Mem(Pol.Command, x, cs), h: {Laws.implies(s, Laws.backs(req, hist, x)) == True{} : Bool}) -> {Laws.implies(s, Laws.backed(req, hist, cs)) == True{} : Bool}:
  match cs:
    case Nil{}:
      match m:
    case y <> t:
      match m:
        case Inl{e}:
          %Equal.sym(Pol.Command, y, x, e) : {Laws.implies(s, Bool.or(Laws.backs(req, hist, _), Laws.backed(req, hist, t))) == True{} : Bool}
          implies_or_l(s, Laws.backs(req, hist, x), Laws.backed(req, hist, t), h)
        case Inr{n}:
          implies_or_r(s, Laws.backs(req, hist, y), Laws.backed(req, hist, t), success_after_commit.backed_found(s, req, hist, t, x, n, h))

def success_after_commit.committed_found(+s: Bool, +b: U32, +req: U32, +op: Req.Op, es: List<&2, Pol.HostEvent>, -x: Pol.HostEvent, m: Mem(Pol.HostEvent, x, es), h: {Laws.implies(s, Laws.commits_at(b, req, op, x)) == True{} : Bool}) -> {Laws.implies(s, Laws.committed_in(b, req, op, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      match m:
    case y <> t:
      match m:
        case Inl{e}:
          %Equal.sym(Pol.HostEvent, y, x, e) : {Laws.implies(s, Bool.or(Laws.commits_at(b, req, op, _), Laws.committed_in(b, req, op, t))) == True{} : Bool}
          implies_or_l(s, Laws.commits_at(b, req, op, x), Laws.committed_in(b, req, op, t), h)
        case Inr{n}:
          implies_or_r(s, Laws.commits_at(b, req, op, y), Laws.committed_in(b, req, op, t), success_after_commit.committed_found(s, b, req, op, t, x, n, h))

def success_after_commit.backs_read(+s: Bool, +req: U32, -batch: U32, -op: Req.Op, -hist: List<&2, Pol.HostEvent>, hw: {Req.is_write(Req.Run{op}) == False{} : Bool}) -> {Laws.implies(s, Laws.backs(req, hist, Pol.Queue{batch, req, op})) == True{} : Bool}:
  %Equal.sym(Bool, U32.is_eq(req, req), True{}, u32_eq_refl(req)) : {Laws.implies(s, Bool.and(_, Laws.implies(Req.is_write(Req.Run{op}), Laws.committed_in(batch, req, op, hist)))) == True{} : Bool}
  %Equal.sym(Bool, Req.is_write(Req.Run{op}), False{}, hw) : {Laws.implies(s, Laws.implies(_, Laws.committed_in(batch, req, op, hist))) == True{} : Bool}
  implies_true_r(s)

def success_after_commit.backs_write(-s: Bool, +req: U32, -batch: U32, -op: Req.Op, -hist: List<&2, Pol.HostEvent>, hw: {Req.is_write(Req.Run{op}) == True{} : Bool}, hc: {Laws.implies(s, Laws.committed_in(batch, req, op, hist)) == True{} : Bool}) -> {Laws.implies(s, Laws.backs(req, hist, Pol.Queue{batch, req, op})) == True{} : Bool}:
  %Equal.sym(Bool, U32.is_eq(req, req), True{}, u32_eq_refl(req)) : {Laws.implies(s, Bool.and(_, Laws.implies(Req.is_write(Req.Run{op}), Laws.committed_in(batch, req, op, hist)))) == True{} : Bool}
  %Equal.sym(Bool, Req.is_write(Req.Run{op}), True{}, hw) : {Laws.implies(s, Laws.implies(_, Laws.committed_in(batch, req, op, hist))) == True{} : Bool}
  hc

def success_after_commit.at_synced(-s: Bool, -batch: U32, -b: U32, -req: U32, -op: Req.Op, -outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -u: U32, heq: {U32.is_eq(batch, b) == True{} : Bool}, h: {Laws.implies(s, Laws.reports_commit(outs, req, op)) == True{} : Bool}) -> {Laws.implies(s, Laws.commits_at(batch, req, op, Pol.Synced{b, True{}, outs, u})) == True{} : Bool}:
  %Equal.sym(Bool, U32.is_eq(batch, b), True{}, heq) : {Laws.implies(s, Bool.and(_, Bool.and(True{}, Laws.reports_commit(outs, req, op)))) == True{} : Bool}
  h

def success_after_commit.rc_post.commit(c: Bool, +o: Rsp.Outcome, +uid: U32, +name: String, +text: String, +u: U32, -x: Bool, ec: {Rsp.is_committed(o) == c : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), Bool.or(Rsp.is_committed(o), x)) == True{} : Bool}:
  match c:
    case True{}:
      %Equal.sym(Bool, Rsp.is_committed(o), True{}, ec) : {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), Bool.or(_, x)) == True{} : Bool}
      implies_true_r(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)))
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), False{}, Laws.created_needs_commit(u, uid, name, text, o, ec)) : {Laws.implies(_, Bool.or(Rsp.is_committed(o), x)) == True{} : Bool}
      {==}

def success_after_commit.rc_post.pick(hit: Bool, +o: Rsp.Outcome, +uid: U32, +name: String, +text: String, +u: U32, -f: Rsp.Outcome, -x: Bool, ih: {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, f, u)), x) == True{} : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.find.pick(hit, o, f), u)), Bool.or(Bool.and(hit, Rsp.is_committed(o)), x)) == True{} : Bool}:
  match hit:
    case True{}:
      success_after_commit.rc_post.commit(Rsp.is_committed(o), o, uid, name, text, u, x, {==})
    case False{}:
      ih

def success_after_commit.rc_post(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +name: String, +text: String, +u: U32) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.find(outs, req), u)), Laws.reports_commit(outs, req, Req.InsertPost{uid, name, text})) == True{} : Bool}:
  match outs:
    case Nil{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(Req.InsertPost{uid, name, text}, u)) : {Laws.implies(_, False{}) == True{} : Bool}
      {==}
    case (r, o) <> +rest:
      success_after_commit.rc_post.pick(U32.is_eq(r, req), o, uid, name, text, u, Pol.find(rest, req), Laws.reports_commit(rest, req, Req.InsertPost{uid, name, text}), success_after_commit.rc_post(rest, req, uid, name, text, u))

def success_after_commit.rc_like.commit(c: Bool, +o: Rsp.Outcome, +uid: U32, +pid: U32, +u: U32, -x: Bool, ec: {Rsp.is_liked(o) == c : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), Bool.or(Rsp.is_liked(o), x)) == True{} : Bool}:
  match c:
    case True{}:
      %Equal.sym(Bool, Rsp.is_liked(o), True{}, ec) : {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), Bool.or(_, x)) == True{} : Bool}
      implies_true_r(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)))
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), False{}, Laws.liked_needs_commit(u, uid, pid, o, ec)) : {Laws.implies(_, Bool.or(Rsp.is_liked(o), x)) == True{} : Bool}
      {==}

def success_after_commit.rc_like.pick(hit: Bool, +o: Rsp.Outcome, +uid: U32, +pid: U32, +u: U32, -f: Rsp.Outcome, -x: Bool, ih: {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, f, u)), x) == True{} : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.find.pick(hit, o, f), u)), Bool.or(Bool.and(hit, Rsp.is_liked(o)), x)) == True{} : Bool}:
  match hit:
    case True{}:
      success_after_commit.rc_like.commit(Rsp.is_liked(o), o, uid, pid, u, x, {==})
    case False{}:
      ih

def success_after_commit.rc_like(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +pid: U32, +u: U32) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.find(outs, req), u)), Laws.reports_commit(outs, req, Req.InsertLike{uid, pid})) == True{} : Bool}:
  match outs:
    case Nil{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(Req.InsertLike{uid, pid}, u)) : {Laws.implies(_, False{}) == True{} : Bool}
      {==}
    case (r, o) <> +rest:
      success_after_commit.rc_like.pick(U32.is_eq(r, req), o, uid, pid, u, Pol.find(rest, req), Laws.reports_commit(rest, req, Req.InsertLike{uid, pid}), success_after_commit.rc_like(rest, req, uid, pid, u))

def success_after_commit.sb_append(+hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, +xs: List<&2, Pol.Command>, +ys: List<&2, Pol.Command>, +hx: {Laws.success_backed(hist, cs, xs) == True{} : Bool}, hy: {Laws.success_backed(hist, cs, ys) == True{} : Bool}) -> {Laws.success_backed(hist, cs, List.append(&2, Pol.Command, xs, ys)) == True{} : Bool}:
  match xs:
    case Nil{}:
      hy
    case x <> rest:
      match x:
        case Pol.Send{req, conn, gen, reply}:
          and_intro(Laws.implies(Rsp.is_success(reply), Laws.backed(req, hist, cs)), Laws.success_backed(hist, cs, List.append(&2, Pol.Command, rest, ys)), and_l(Laws.implies(Rsp.is_success(reply), Laws.backed(req, hist, cs)), Laws.success_backed(hist, cs, rest), hx), success_after_commit.sb_append(hist, cs, rest, ys, and_r(Laws.implies(Rsp.is_success(reply), Laws.backed(req, hist, cs)), Laws.success_backed(hist, cs, rest), hx), hy))
        case Pol.Queue{b, req, op}:
          success_after_commit.sb_append(hist, cs, rest, ys, hx, hy)
        case Pol.Open{b, k}:
          success_after_commit.sb_append(hist, cs, rest, ys, hx, hy)

def success_after_commit.mem_run_acc(-x: Pol.Command, es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, m: Mem(Pol.Command, x, acc)) -> Mem(Pol.Command, x, Pol.commands(Pol.run.go(es, t, acc))):
  match es:
    case Nil{}:
      match t:
        case (s, c):
          mem_append_l(Pol.Command, x, acc, c, m)
    case e <> rest:
      match t:
        case (s, +c):
          success_after_commit.mem_run_acc(x, rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), mem_append_l(Pol.Command, x, acc, c, m))

def success_after_commit.mem_run_now(-x: Pol.Command, es: List<&2, Pol.HostEvent>, +s: Pol.State, +c: List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, m: Mem(Pol.Command, x, c)) -> Mem(Pol.Command, x, Pol.commands(Pol.run.go(es, (s, c), acc))):
  match es:
    case Nil{}:
      mem_append_r(Pol.Command, x, acc, c, m)
    case e <> rest:
      success_after_commit.mem_run_acc(x, rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), mem_append_r(Pol.Command, x, acc, c, m))

def success_after_commit.reject_send(+status: U32, +msg: String, -req: U32, -conn: U32, -gen: U32, -hist: List<&2, Pol.HostEvent>, -cs: List<&2, Pol.Command>, hd: {Bool.not(Rsp.is_success(Rsp.Error{status, msg})) == True{} : Bool}) -> {Laws.success_backed(hist, cs, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}]) == True{} : Bool}:
  %Equal.sym(Bool, Rsp.is_success(Rsp.Error{status, msg}), False{}, not_true(Rsp.is_success(Rsp.Error{status, msg}), hd)) : {Bool.and(Laws.implies(_, Laws.backed(req, hist, cs)), True{}) == True{} : Bool}
  {==}

def success_after_commit.AllQ_es(+cs: List<&2, Pol.Command>, +id: U32, es: List<&2, Pol.Pending>) -> Data:
  match es:
    case Nil{}:
      Unit
    case p <> rest:
      match p:
        case Pol.Pending{req, conn, gen, op}:
          +Sigma<Mem(Pol.Command, Pol.Queue{id, req, op}, cs), _ => success_after_commit.AllQ_es(cs, id, rest)>

def success_after_commit.AllQ_bt(+cs: List<&2, Pol.Command>, bt: Pol.Batch) -> Data:
  match bt:
    case Pol.Batch{id, kind, entries}:
      success_after_commit.AllQ_es(cs, id, entries)

def success_after_commit.AllQ_bs(+cs: List<&2, Pol.Command>, bs: List<&2, Pol.Batch>) -> Data:
  match bs:
    case Nil{}:
      Unit
    case bt <> rest:
      +Sigma<success_after_commit.AllQ_bt(cs, bt), _ => success_after_commit.AllQ_bs(cs, rest)>

def success_after_commit.Inv(+hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>) -> Data:
  match t:
    case (s, c):
      match s:
        case Pol.State{next, reads, writes, inflight}:
          +Sigma<{Laws.success_backed(hist, cs, List.append(&2, Pol.Command, acc, c)) == True{} : Bool}, _ => +Sigma<success_after_commit.AllQ_bt(cs, reads), _ => +Sigma<success_after_commit.AllQ_bt(cs, writes), _ => success_after_commit.AllQ_bs(cs, inflight)>>>

# The outcome of an entry on a Synced with ok True{} is head(left, outs,
# req): the pair at the head of left when its id is req, else find(outs,
# req). A success then needs the commit: for find, rc_post and rc_like
# (S09); for the head of left, that pair is a pair of outs (AllIn), so
# outs reports the commit (mem_rc).

def success_after_commit.mem_rc(all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -r: U32, -o: Rsp.Outcome, +req: U32, +op: Req.Op, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), heq: {U32.is_eq(r, req) == True{} : Bool}, hc: {Laws.commits(op, o) == True{} : Bool}) -> {Laws.reports_commit(all, req, op) == True{} : Bool}:
  match all:
    case Nil{}:
      match m:
    case (+r2, +o2) <> +t:
      match m:
        case Inl{e}:
          %Equal.sym(+Sigma<U32, _ => Rsp.Outcome>, (r2, o2), (r, o), e) : {Laws.reports_commit(_ <> t, req, op) == True{} : Bool}
          %Equal.sym(Bool, U32.is_eq(r, req), True{}, heq) : {Bool.or(Bool.and(_, Laws.commits(op, o)), Laws.reports_commit(t, req, op)) == True{} : Bool}
          %Equal.sym(Bool, Laws.commits(op, o), True{}, hc) : {Bool.or(Bool.and(True{}, _), Laws.reports_commit(t, req, op)) == True{} : Bool}
          {==}
        case Inr{n}:
          %Equal.sym(Bool, Laws.reports_commit(t, req, op), True{}, success_after_commit.mem_rc(t, r, o, req, op, n, heq, hc)) : {Bool.or(Bool.and(U32.is_eq(r2, req), Laws.commits(op, o2)), _) == True{} : Bool}
          or_true_r(Bool.and(U32.is_eq(r2, req), Laws.commits(op, o2)))

def success_after_commit.rc_hit.post(c: Bool, -r: U32, +o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +name: String, +text: String, +u: U32, heq: {U32.is_eq(r, req) == True{} : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), ec: {Rsp.is_committed(o) == c : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), Laws.reports_commit(all, req, Req.InsertPost{uid, name, text})) == True{} : Bool}:
  match c:
    case True{}:
      %Equal.sym(Bool, Laws.reports_commit(all, req, Req.InsertPost{uid, name, text}), True{}, success_after_commit.mem_rc(all, r, o, req, Req.InsertPost{uid, name, text}, m, heq, ec)) : {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), _) == True{} : Bool}
      implies_true_r(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)))
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, o, u)), False{}, Laws.created_needs_commit(u, uid, name, text, o, ec)) : {Laws.implies(_, Laws.reports_commit(all, req, Req.InsertPost{uid, name, text})) == True{} : Bool}
      {==}

def success_after_commit.rc_head.post.pick(hit: Bool, -r: U32, +o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +name: String, +text: String, +u: U32, heq: {U32.is_eq(r, req) == hit : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all)) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head.pick(hit, o, all, req), u)), Laws.reports_commit(all, req, Req.InsertPost{uid, name, text})) == True{} : Bool}:
  match hit:
    case True{}:
      success_after_commit.rc_hit.post(Rsp.is_committed(o), r, o, all, req, uid, name, text, u, heq, m, {==})
    case False{}:
      success_after_commit.rc_post(all, req, uid, name, text, u)

def success_after_commit.rc_head.post(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +name: String, +text: String, +u: U32, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head(left, all, req), u)), Laws.reports_commit(all, req, Req.InsertPost{uid, name, text})) == True{} : Bool}:
  match left:
    case Nil{}:
      success_after_commit.rc_post(all, req, uid, name, text, u)
    case (+r, +o) <> rest:
      match hl:
        case (m, hr):
          success_after_commit.rc_head.post.pick(U32.is_eq(r, req), r, o, all, req, uid, name, text, u, {==}, m)

def success_after_commit.rc_hit.like(c: Bool, -r: U32, +o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +pid: U32, +u: U32, heq: {U32.is_eq(r, req) == True{} : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), ec: {Rsp.is_liked(o) == c : Bool}) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), Laws.reports_commit(all, req, Req.InsertLike{uid, pid})) == True{} : Bool}:
  match c:
    case True{}:
      %Equal.sym(Bool, Laws.reports_commit(all, req, Req.InsertLike{uid, pid}), True{}, success_after_commit.mem_rc(all, r, o, req, Req.InsertLike{uid, pid}, m, heq, ec)) : {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), _) == True{} : Bool}
      implies_true_r(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)))
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, o, u)), False{}, Laws.liked_needs_commit(u, uid, pid, o, ec)) : {Laws.implies(_, Laws.reports_commit(all, req, Req.InsertLike{uid, pid})) == True{} : Bool}
      {==}

def success_after_commit.rc_head.like.pick(hit: Bool, -r: U32, +o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +pid: U32, +u: U32, heq: {U32.is_eq(r, req) == hit : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all)) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head.pick(hit, o, all, req), u)), Laws.reports_commit(all, req, Req.InsertLike{uid, pid})) == True{} : Bool}:
  match hit:
    case True{}:
      success_after_commit.rc_hit.like(Rsp.is_liked(o), r, o, all, req, uid, pid, u, heq, m, {==})
    case False{}:
      success_after_commit.rc_like(all, req, uid, pid, u)

def success_after_commit.rc_head.like(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +uid: U32, +pid: U32, +u: U32, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head(left, all, req), u)), Laws.reports_commit(all, req, Req.InsertLike{uid, pid})) == True{} : Bool}:
  match left:
    case Nil{}:
      success_after_commit.rc_like(all, req, uid, pid, u)
    case (+r, +o) <> rest:
      match hl:
        case (m, hr):
          success_after_commit.rc_head.like.pick(U32.is_eq(r, req), r, o, all, req, uid, pid, u, {==}, m)


def success_after_commit.reply(ok: Bool, +op: Req.Op, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +u: U32, +batch: U32, +b: U32, heq: {U32.is_eq(batch, b) == True{} : Bool}, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, m: Mem(Pol.HostEvent, Pol.Synced{b, ok, all, u}, hist), mq: Mem(Pol.Command, Pol.Queue{batch, req, op}, cs), hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), u)), Laws.backed(req, hist, cs)) == True{} : Bool}:
  match ok:
    case False{}:
      %Equal.sym(Bool, Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(op, u)) : {Laws.implies(_, Laws.backed(req, hist, cs)) == True{} : Bool}
      {==}
    case True{}:
      match op:
        case Req.SelectOne{}:
          success_after_commit.backed_found(Rsp.is_success(Rsp.respond(Req.SelectOne{}, Pol.head(left, all, req), u)), req, hist, cs, Pol.Queue{batch, req, Req.SelectOne{}}, mq, success_after_commit.backs_read(Rsp.is_success(Rsp.respond(Req.SelectOne{}, Pol.head(left, all, req), u)), req, batch, Req.SelectOne{}, hist, {==}))
        case Req.FeedQuery{}:
          success_after_commit.backed_found(Rsp.is_success(Rsp.respond(Req.FeedQuery{}, Pol.head(left, all, req), u)), req, hist, cs, Pol.Queue{batch, req, Req.FeedQuery{}}, mq, success_after_commit.backs_read(Rsp.is_success(Rsp.respond(Req.FeedQuery{}, Pol.head(left, all, req), u)), req, batch, Req.FeedQuery{}, hist, {==}))
        case Req.PostQuery{pid}:
          success_after_commit.backed_found(Rsp.is_success(Rsp.respond(Req.PostQuery{pid}, Pol.head(left, all, req), u)), req, hist, cs, Pol.Queue{batch, req, Req.PostQuery{pid}}, mq, success_after_commit.backs_read(Rsp.is_success(Rsp.respond(Req.PostQuery{pid}, Pol.head(left, all, req), u)), req, batch, Req.PostQuery{pid}, hist, {==}))
        case Req.InsertPost{uid, name, text}:
          success_after_commit.backed_found(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head(left, all, req), u)), req, hist, cs, Pol.Queue{batch, req, Req.InsertPost{uid, name, text}}, mq, success_after_commit.backs_write(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head(left, all, req), u)), req, batch, Req.InsertPost{uid, name, text}, hist, {==}, success_after_commit.committed_found(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head(left, all, req), u)), batch, req, Req.InsertPost{uid, name, text}, hist, Pol.Synced{b, True{}, all, u}, m, success_after_commit.at_synced(Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, Pol.head(left, all, req), u)), batch, b, req, Req.InsertPost{uid, name, text}, all, u, heq, success_after_commit.rc_head.post(left, all, req, uid, name, text, u, hl)))))
        case Req.InsertLike{uid, pid}:
          success_after_commit.backed_found(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head(left, all, req), u)), req, hist, cs, Pol.Queue{batch, req, Req.InsertLike{uid, pid}}, mq, success_after_commit.backs_write(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head(left, all, req), u)), req, batch, Req.InsertLike{uid, pid}, hist, {==}, success_after_commit.committed_found(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head(left, all, req), u)), batch, req, Req.InsertLike{uid, pid}, hist, Pol.Synced{b, True{}, all, u}, m, success_after_commit.at_synced(Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, Pol.head(left, all, req), u)), batch, b, req, Req.InsertLike{uid, pid}, all, u, heq, success_after_commit.rc_head.like(left, all, req, uid, pid, u, hl)))))


def success_after_commit.skip.pick(hit: Bool, -rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -later: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all), hs: AllIn(+Sigma<U32, _ => Rsp.Outcome>, later, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip.pick(hit, rest, later), all):
  match hit:
    case True{}:
      hr
    case False{}:
      hs

def success_after_commit.skip(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, +hr):
          success_after_commit.skip.pick(U32.is_eq(r, req), rest, Pol.skip(rest, req), all, hr, success_after_commit.skip(rest, req, all, hr))

def success_after_commit.after.pick(hit: Bool, rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after.pick(hit, rest, req), all):
  match hit:
    case True{}:
      hr
    case False{}:
      success_after_commit.skip(rest, req, all, hr)

def success_after_commit.after(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, hr):
          success_after_commit.after.pick(U32.is_eq(r, req), rest, req, all, hr)

# The replies of a Synced and of a Died.

def success_after_commit.answer(+id: U32, +b: U32, +heq: {U32.is_eq(id, b) == True{} : Bool}, +ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, +m: Mem(Pol.HostEvent, Pol.Synced{b, ok, all, u}, hist), hq: success_after_commit.AllQ_es(cs, id, es), +hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all), ih: {Laws.success_backed(hist, cs, rest) == True{} : Bool}) -> {Laws.success_backed(hist, cs, Pol.answer(ok, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, +op}:
          match hq:
            case (mq, hq2):
              and_intro(Laws.implies(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), u)), Laws.backed(req, hist, cs)), Laws.success_backed(hist, cs, Pol.answer(ok, all, u, tail, Pol.after(left, req), rest)), success_after_commit.reply(ok, op, all, left, req, u, id, b, heq, hist, cs, m, mq, hl), success_after_commit.answer(id, b, heq, ok, all, u, hist, cs, tail, Pol.after(left, req), rest, m, hq2, success_after_commit.after(left, req, all, hl), ih))

def success_after_commit.answer_in.put(hit: Bool, bt: Pol.Batch, +b: U32, heq: {U32.is_eq(Pol.id_of(bt), b) == hit : Bool}, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, -rest: List<&2, Pol.Command>, +m: Mem(Pol.HostEvent, Pol.Synced{b, ok, outs, u}, hist), hb: success_after_commit.AllQ_bt(cs, bt), ih: {Laws.success_backed(hist, cs, rest) == True{} : Bool}) -> {Laws.success_backed(hist, cs, Pol.answer_in.put(hit, bt, ok, outs, u, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{+id, kind, entries}:
          success_after_commit.answer(id, b, heq, ok, outs, u, hist, cs, entries, outs, rest, m, hb, all_in_self(+Sigma<U32, _ => Rsp.Outcome>, outs), ih)
    case False{}:
      ih

def success_after_commit.answer_in(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, bs: List<&2, Pol.Batch>, +m: Mem(Pol.HostEvent, Pol.Synced{b, ok, outs, u}, hist), hq: success_after_commit.AllQ_bs(cs, bs)) -> {Laws.success_backed(hist, cs, Pol.answer_in(b, ok, outs, u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      match hq:
        case (hb, hr):
          success_after_commit.answer_in.put(U32.is_eq(Pol.id_of(bt), b), bt, b, {==}, ok, outs, u, hist, cs, Pol.answer_in(b, ok, outs, u, rest), m, hb, success_after_commit.answer_in(b, ok, outs, u, hist, cs, rest, m, hr))

def success_after_commit.fail(+all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, ih: {Laws.success_backed(hist, cs, rest) == True{} : Bool}) -> {Laws.success_backed(hist, cs, Pol.answer(False{}, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, +op}:
          %Equal.sym(Bool, Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, u)), False{}, Laws.no_success_on_unknown(op, u)) : {Bool.and(Laws.implies(_, Laws.backed(req, hist, cs)), Laws.success_backed(hist, cs, Pol.answer(False{}, all, u, tail, Pol.after(left, req), rest))) == True{} : Bool}
          success_after_commit.fail(all, u, hist, cs, tail, Pol.after(left, req), rest, ih)

def success_after_commit.fail_in.put(hit: Bool, bt: Pol.Batch, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, -rest: List<&2, Pol.Command>, ih: {Laws.success_backed(hist, cs, rest) == True{} : Bool}) -> {Laws.success_backed(hist, cs, Pol.answer_in.put(hit, bt, False{}, outs, u, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          success_after_commit.fail(outs, u, hist, cs, entries, outs, rest, ih)
    case False{}:
      ih

def success_after_commit.fail_in(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, bs: List<&2, Pol.Batch>) -> {Laws.success_backed(hist, cs, Pol.answer_in(b, False{}, outs, u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      success_after_commit.fail_in.put(U32.is_eq(Pol.id_of(bt), b), bt, outs, u, hist, cs, Pol.answer_in(b, False{}, outs, u, rest), success_after_commit.fail_in(b, outs, u, hist, cs, rest))

def success_after_commit.opened(h: Bool, -n: U32, -k: Pol.BatchKind, -hist: List<&2, Pol.HostEvent>, -cs: List<&2, Pol.Command>, -rest: List<&2, Pol.Command>, ih: {Laws.success_backed(hist, cs, rest) == True{} : Bool}) -> {Laws.success_backed(hist, cs, Pol.opened(h, n, k, rest)) == True{} : Bool}:
  match h:
    case True{}:
      ih
    case False{}:
      ih

# The Queues of the batches: fresh, park, keep and drop keep them.

def success_after_commit.fresh(h: Bool, -n: U32, -k: Pol.BatchKind, -cs: List<&2, Pol.Command>, -bt: Pol.Batch, hb: success_after_commit.AllQ_bt(cs, bt)) -> success_after_commit.AllQ_bt(cs, Pol.fresh(h, n, k, bt)):
  match h:
    case True{}:
      Unit{}
    case False{}:
      hb

def success_after_commit.park(h: Bool, -cs: List<&2, Pol.Command>, -bt: Pol.Batch, -bs: List<&2, Pol.Batch>, hb: success_after_commit.AllQ_bt(cs, bt), hs: success_after_commit.AllQ_bs(cs, bs)) -> success_after_commit.AllQ_bs(cs, Pol.park(h, bt, bs)):
  match h:
    case True{}:
      (hb, hs)
    case False{}:
      hs

def success_after_commit.keep.put(hit: Bool, -bt: Pol.Batch, -cs: List<&2, Pol.Command>, -rest: List<&2, Pol.Batch>, hb: success_after_commit.AllQ_bt(cs, bt), ih: success_after_commit.AllQ_bs(cs, rest)) -> success_after_commit.AllQ_bs(cs, Pol.keep.put(hit, bt, rest)):
  match hit:
    case True{}:
      ih
    case False{}:
      (hb, ih)

def success_after_commit.keep(+b: U32, +cs: List<&2, Pol.Command>, bs: List<&2, Pol.Batch>, hq: success_after_commit.AllQ_bs(cs, bs)) -> success_after_commit.AllQ_bs(cs, Pol.keep(b, bs)):
  match bs:
    case Nil{}:
      Unit{}
    case +bt <> +rest:
      match hq:
        case (hb, hr):
          success_after_commit.keep.put(U32.is_eq(Pol.id_of(bt), b), bt, cs, Pol.keep(b, rest), hb, success_after_commit.keep(b, cs, rest, hr))

def success_after_commit.drop.put(hit: Bool, -req: U32, -conn: U32, -gen: U32, -op: Req.Op, -id: U32, -cs: List<&2, Pol.Command>, -rest: List<&2, Pol.Pending>, mq: Mem(Pol.Command, Pol.Queue{id, req, op}, cs), ih: success_after_commit.AllQ_es(cs, id, rest)) -> success_after_commit.AllQ_es(cs, id, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, rest)):
  match hit:
    case True{}:
      ih
    case False{}:
      (mq, ih)

def success_after_commit.drop(+cg: U32, +gg: U32, +cs: List<&2, Pol.Command>, +id: U32, es: List<&2, Pol.Pending>, hq: success_after_commit.AllQ_es(cs, id, es)) -> success_after_commit.AllQ_es(cs, id, Pol.drop(cg, gg, es)):
  match es:
    case Nil{}:
      Unit{}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          match hq:
            case (mq, hr):
              success_after_commit.drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), req, conn, gen, op, id, cs, Pol.drop(cg, gg, rest), mq, success_after_commit.drop(cg, gg, cs, id, rest, hr))

def success_after_commit.drop_bt(+cg: U32, +gg: U32, +cs: List<&2, Pol.Command>, bt: Pol.Batch, hb: success_after_commit.AllQ_bt(cs, bt)) -> success_after_commit.AllQ_bt(cs, Pol.drop_batch(cg, gg, bt)):
  match bt:
    case Pol.Batch{+id, kind, entries}:
      success_after_commit.drop(cg, gg, cs, id, entries, hb)

def success_after_commit.drop_all(+cg: U32, +gg: U32, +cs: List<&2, Pol.Command>, bs: List<&2, Pol.Batch>, hq: success_after_commit.AllQ_bs(cs, bs)) -> success_after_commit.AllQ_bs(cs, Pol.drop_all(cg, gg, bs)):
  match bs:
    case Nil{}:
      Unit{}
    case +bt <> +rest:
      match hq:
        case (hb, hr):
          (success_after_commit.drop_bt(cg, gg, cs, bt, hb), success_after_commit.drop_all(cg, gg, cs, rest, hr))

# One step.

def success_after_commit.put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hist: List<&2, Pol.HostEvent>, h1: {Laws.success_backed(hist, success_after_commit.CS(rest, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x), x) == True{} : Bool}, qr: success_after_commit.AllQ_bt(success_after_commit.CS(rest, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x), reads), qw: success_after_commit.AllQ_bt(success_after_commit.CS(rest, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x), writes), qi: success_after_commit.AllQ_bs(success_after_commit.CS(rest, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x), inflight)) -> success_after_commit.Inv(hist, success_after_commit.CS(rest, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x), (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), x):
  match k:
    case Pol.Read{}:
      match reads:
        case Pol.Batch{+rid, +rk, +res}:
          (success_after_commit.sb_append(hist, success_after_commit.CS(rest, (Pol.State{next, Pol.Batch{rid, rk, Pol.Pending{req, conn, gen, op} <> res}, writes, inflight}, [Pol.Queue{rid, req, op}]), x), x, [Pol.Queue{rid, req, op}], h1, {==}), (success_after_commit.mem_run_now(Pol.Queue{rid, req, op}, rest, Pol.State{next, Pol.Batch{rid, rk, Pol.Pending{req, conn, gen, op} <> res}, writes, inflight}, [Pol.Queue{rid, req, op}], x, Inl{{==}}), qr), qw, qi)
    case Pol.Write{}:
      match writes:
        case Pol.Batch{+wid, +wk, +wes}:
          (success_after_commit.sb_append(hist, success_after_commit.CS(rest, (Pol.State{next, reads, Pol.Batch{wid, wk, Pol.Pending{req, conn, gen, op} <> wes}, inflight}, [Pol.Queue{wid, req, op}]), x), x, [Pol.Queue{wid, req, op}], h1, {==}), qr, (success_after_commit.mem_run_now(Pol.Queue{wid, req, op}, rest, Pol.State{next, reads, Pol.Batch{wid, wk, Pol.Pending{req, conn, gen, op} <> wes}, inflight}, [Pol.Queue{wid, req, op}], x, Inl{{==}}), qw), qi)

def success_after_commit.arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, +x: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hist: List<&2, Pol.HostEvent>, h1: {Laws.success_backed(hist, success_after_commit.CS(rest, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x), x) == True{} : Bool}, qr: success_after_commit.AllQ_bt(success_after_commit.CS(rest, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x), reads), qw: success_after_commit.AllQ_bt(success_after_commit.CS(rest, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x), writes), qi: success_after_commit.AllQ_bs(success_after_commit.CS(rest, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x), inflight), hd: {success_after_commit.reject_fails(d) == True{} : Bool}) -> success_after_commit.Inv(hist, success_after_commit.CS(rest, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x), (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), x):
  match d:
    case Req.Reject{+status, +msg}:
      (success_after_commit.sb_append(hist, success_after_commit.CS(rest, (Pol.State{next, reads, writes, inflight}, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}]), x), x, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}], h1, success_after_commit.reject_send(status, msg, req, conn, gen, hist, success_after_commit.CS(rest, (Pol.State{next, reads, writes, inflight}, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}]), x), hd)), qr, qw, qi)
    case Req.Run{+op}:
      success_after_commit.put(Pol.kind_of(op), req, conn, gen, op, next, reads, writes, inflight, x, rest, hist, h1, qr, qw, qi)

def success_after_commit.flush(+hr: Bool, +hw: Bool, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, h1: {Laws.success_backed(hist, cs, x) == True{} : Bool}, +qr: success_after_commit.AllQ_bt(cs, reads), +qw: success_after_commit.AllQ_bt(cs, writes), +qi: success_after_commit.AllQ_bs(cs, inflight)) -> success_after_commit.Inv(hist, cs, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), x):
  (success_after_commit.sb_append(hist, cs, x, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{})), h1, success_after_commit.opened(hr, next, Pol.Read{}, hist, cs, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}), success_after_commit.opened(hw, Pol.bump(hr, next), Pol.Write{}, hist, cs, Nil{}, {==}))), success_after_commit.fresh(hr, next, Pol.Read{}, cs, reads, qr), success_after_commit.fresh(hw, Pol.bump(hr, next), Pol.Write{}, cs, writes, qw), success_after_commit.park(hw, cs, writes, Pol.park(hr, reads, inflight), qw, success_after_commit.park(hr, cs, reads, inflight, qr, qi)))

def success_after_commit.settle(+hr: Bool, +hw: Bool, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, +m: Mem(Pol.HostEvent, Pol.Synced{b, ok, outs, u}, hist), h1: {Laws.success_backed(hist, cs, x) == True{} : Bool}, +qr: success_after_commit.AllQ_bt(cs, reads), +qw: success_after_commit.AllQ_bt(cs, writes), +qi: success_after_commit.AllQ_bs(cs, inflight)) -> success_after_commit.Inv(hist, cs, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), x):
  (success_after_commit.sb_append(hist, cs, x, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))))), h1, success_after_commit.opened(hr, next, Pol.Read{}, hist, cs, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), success_after_commit.opened(hw, Pol.bump(hr, next), Pol.Write{}, hist, cs, Pol.answer_in(b, ok, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))), success_after_commit.answer_in(b, ok, outs, u, hist, cs, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), m, success_after_commit.park(hw, cs, writes, Pol.park(hr, reads, inflight), qw, success_after_commit.park(hr, cs, reads, inflight, qr, qi)))))), success_after_commit.fresh(hr, next, Pol.Read{}, cs, reads, qr), success_after_commit.fresh(hw, Pol.bump(hr, next), Pol.Write{}, cs, writes, qw), success_after_commit.keep(b, cs, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), success_after_commit.park(hw, cs, writes, Pol.park(hr, reads, inflight), qw, success_after_commit.park(hr, cs, reads, inflight, qr, qi))))

def success_after_commit.died(+hr: Bool, +hw: Bool, +b: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +x: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, h1: {Laws.success_backed(hist, cs, x) == True{} : Bool}, +qr: success_after_commit.AllQ_bt(cs, reads), +qw: success_after_commit.AllQ_bt(cs, writes), +qi: success_after_commit.AllQ_bs(cs, inflight)) -> success_after_commit.Inv(hist, cs, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, Nil{}, 0, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), x):
  (success_after_commit.sb_append(hist, cs, x, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, Nil{}, 0, Pol.park(hw, writes, Pol.park(hr, reads, inflight))))), h1, success_after_commit.opened(hr, next, Pol.Read{}, hist, cs, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, Nil{}, 0, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), success_after_commit.opened(hw, Pol.bump(hr, next), Pol.Write{}, hist, cs, Pol.answer_in(b, False{}, Nil{}, 0, Pol.park(hw, writes, Pol.park(hr, reads, inflight))), success_after_commit.fail_in(b, Nil{}, 0, hist, cs, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), success_after_commit.fresh(hr, next, Pol.Read{}, cs, reads, qr), success_after_commit.fresh(hw, Pol.bump(hr, next), Pol.Write{}, cs, writes, qw), success_after_commit.keep(b, cs, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), success_after_commit.park(hw, cs, writes, Pol.park(hr, reads, inflight), qw, success_after_commit.park(hr, cs, reads, inflight, qr, qi))))

def success_after_commit.step(+s: Pol.State, +e: Pol.HostEvent, +c: List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hist: List<&2, Pol.HostEvent>, m: Mem(Pol.HostEvent, e, hist), hinv: success_after_commit.Inv(hist, success_after_commit.CS(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c)), (s, c), acc)) -> success_after_commit.Inv(hist, success_after_commit.CS(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c)), Pol.step(s, e), List.append(&2, Pol.Command, acc, c)):
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, +r}:
          match hinv:
            case (h1, qr, qw, qi):
              success_after_commit.arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, List.append(&2, Pol.Command, acc, c), rest, hist, h1, qr, qw, qi, success_after_commit.classify_fails(r))
        case Pol.Flushed{+b}:
          match hinv:
            case (h1, qr, qw, qi):
              success_after_commit.flush(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Flushed{b}), List.append(&2, Pol.Command, acc, c)), h1, qr, qw, qi)
        case Pol.Synced{+b, +ok, +outs, +u}:
          match hinv:
            case (h1, qr, qw, qi):
              success_after_commit.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, ok, outs, u, next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Synced{b, ok, outs, u}), List.append(&2, Pol.Command, acc, c)), m, h1, qr, qw, qi)
        case Pol.Died{+b}:
          match hinv:
            case (h1, qr, qw, qi):
              success_after_commit.died(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, next, reads, writes, inflight, List.append(&2, Pol.Command, acc, c), hist, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Died{b}), List.append(&2, Pol.Command, acc, c)), h1, qr, qw, qi)
        case Pol.Gone{+cg, +gg}:
          match hinv:
            case (h1, qr, qw, qi):
              (success_after_commit.sb_append(hist, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Gone{cg, gg}), List.append(&2, Pol.Command, acc, c)), List.append(&2, Pol.Command, acc, c), Nil{}, h1, {==}), success_after_commit.drop_bt(cg, gg, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Gone{cg, gg}), List.append(&2, Pol.Command, acc, c)), reads, qr), success_after_commit.drop_bt(cg, gg, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Gone{cg, gg}), List.append(&2, Pol.Command, acc, c)), writes, qw), success_after_commit.drop_all(cg, gg, success_after_commit.CS(rest, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Gone{cg, gg}), List.append(&2, Pol.Command, acc, c)), inflight, qi))

def success_after_commit.go(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +hist: List<&2, Pol.HostEvent>, hes: AllIn(Pol.HostEvent, es, hist), hinv: success_after_commit.Inv(hist, success_after_commit.CS(es, t, acc), t, acc)) -> {Laws.success_backed(hist, success_after_commit.CS(es, t, acc), Pol.commands(Pol.run.go(es, t, acc))) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (s, c):
          match s:
            case Pol.State{next, reads, writes, inflight}:
              match hinv:
                case (h1, qr, qw, qi):
                  h1
    case e <> rest:
      match t:
        case (+s, +c):
          match hes:
            case (m, hrest):
              success_after_commit.go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), hist, hrest, success_after_commit.step(s, e, c, acc, rest, hist, m, hinv))

def Laws.success_after_commit(es):
  success_after_commit.go(es, (Pol.state0(), Nil{}), Nil{}, es, all_in_self(Pol.HostEvent, es), ({==}, Unit{}, Unit{}, Unit{}))

# one_reply_per_request
# ---------------------
# Fix a request id u. Invariant, for the state (s, c) after a prefix, the
# commands acc before c and the events es still to come: the replies to u in
# acc ++ c, plus the entries for u in the open batches and the batches in
# flight, plus the Arrived events for u in es, is at most Once (all counted
# with saturation). A step turns at most one arrival into a reply or an
# entry, and an entry into at most one reply:
# step_count(step(s, e)) <= pend_count(s) + arrival_of(e). Flushing only
# moves a batch; settling answers the batches with the event's id and keep
# removes exactly those (part_in). The hypothesis of the law is the
# invariant at the start, and at the end of the trace the replies are a
# part of the sum.

def one_reply_per_request.pc_es(+u: U32, es: List<&2, Pol.Pending>) -> Laws.Count:
  match es:
    case Nil{}:
      Laws.Never{}
    case p <> rest:
      match p:
        case Pol.Pending{req, conn, gen, op}:
          Laws.count.add(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, rest))

def one_reply_per_request.pc_bt(+u: U32, bt: Pol.Batch) -> Laws.Count:
  match bt:
    case Pol.Batch{id, kind, entries}:
      one_reply_per_request.pc_es(u, entries)

def one_reply_per_request.pc_bs(+u: U32, bs: List<&2, Pol.Batch>) -> Laws.Count:
  match bs:
    case Nil{}:
      Laws.Never{}
    case bt <> rest:
      Laws.count.add(one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, rest))

def one_reply_per_request.pc_drop.put(hit: Bool, +u: U32, +req: U32, +conn: U32, +gen: U32, -op: Req.Op, +pd: List<&2, Pol.Pending>, +pr: List<&2, Pol.Pending>, ih: {count.le(one_reply_per_request.pc_es(u, pd), one_reply_per_request.pc_es(u, pr)) == True{} : Bool}) -> {count.le(one_reply_per_request.pc_es(u, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, pd)), Laws.count.add(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, pd), one_reply_per_request.pc_es(u, pr), ih)
    case False{}:
      count.both(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, pd), one_reply_per_request.pc_es(u, pr), ih)

def one_reply_per_request.pc_drop(+u: U32, +cg: U32, +gg: U32, es: List<&2, Pol.Pending>) -> {count.le(one_reply_per_request.pc_es(u, Pol.drop(cg, gg, es)), one_reply_per_request.pc_es(u, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          one_reply_per_request.pc_drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), u, req, conn, gen, op, Pol.drop(cg, gg, rest), rest, one_reply_per_request.pc_drop(u, cg, gg, rest))

def one_reply_per_request.pc_drop_bt(+u: U32, +cg: U32, +gg: U32, bt: Pol.Batch) -> {count.le(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, bt)), one_reply_per_request.pc_bt(u, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      one_reply_per_request.pc_drop(u, cg, gg, entries)

def one_reply_per_request.pc_drop_all(+u: U32, +cg: U32, +gg: U32, bs: List<&2, Pol.Batch>) -> {count.le(one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, bs)), one_reply_per_request.pc_bs(u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      count.add_mono(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, bt)), one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, rest)), one_reply_per_request.pc_bs(u, rest), one_reply_per_request.pc_drop_bt(u, cg, gg, bt), one_reply_per_request.pc_drop_all(u, cg, gg, rest))

def one_reply_per_request.pc_dropped(+u: U32, +cg: U32, +gg: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, reads)), Laws.count.add(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, writes)), one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight)))) == True{} : Bool}:
  count.add_mono(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, reads)), one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, writes)), one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, inflight))), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight)), one_reply_per_request.pc_drop_bt(u, cg, gg, reads), count.add_mono(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, writes)), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, inflight)), one_reply_per_request.pc_bs(u, inflight), one_reply_per_request.pc_drop_bt(u, cg, gg, writes), one_reply_per_request.pc_drop_all(u, cg, gg, inflight)))

def one_reply_per_request.pend_count(+u: U32, s: Pol.State) -> Laws.Count:
  match s:
    case Pol.State{next, reads, writes, inflight}:
      Laws.count.add(one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight)))

def one_reply_per_request.step_count(+u: U32, t: Pol.State & List<&2, Pol.Command>) -> Laws.Count:
  match t:
    case (s, c):
      Laws.count.add(Laws.replies(u, c), one_reply_per_request.pend_count(u, s))

def one_reply_per_request.inv(+u: U32, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>, es: List<&2, Pol.HostEvent>) -> Bool:
  match t:
    case (s, c):
      Laws.at_most_once(Laws.count.add(Laws.count.add(Laws.replies(u, List.append(&2, Pol.Command, acc, c)), one_reply_per_request.pend_count(u, s)), Laws.arrivals(u, es)))

def one_reply_per_request.replies_append(+u: U32, +xs: List<&2, Pol.Command>, +ys: List<&2, Pol.Command>) -> {Laws.replies(u, List.append(&2, Pol.Command, xs, ys)) == Laws.count.add(Laws.replies(u, xs), Laws.replies(u, ys)) : Laws.Count}:
  match xs:
    case Nil{}:
      {==}
    case x <> rest:
      %Equal.sym(Laws.Count, Laws.replies(u, List.append(&2, Pol.Command, rest, ys)), Laws.count.add(Laws.replies(u, rest), Laws.replies(u, ys)), one_reply_per_request.replies_append(u, rest, ys)) : {Laws.count.add(Laws.reply_to(u, x), _) == Laws.count.add(Laws.count.add(Laws.reply_to(u, x), Laws.replies(u, rest)), Laws.replies(u, ys)) : Laws.Count}
      %count.assoc(Laws.reply_to(u, x), Laws.replies(u, rest), Laws.replies(u, ys)) : {_ == Laws.count.add(Laws.count.add(Laws.reply_to(u, x), Laws.replies(u, rest)), Laws.replies(u, ys)) : Laws.Count}
      {==}

# answer and keep split the entries of the batches: each entry for u
# becomes a reply or stays, never both.

def one_reply_per_request.answer_count(+u: U32, +ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +uu: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +ra: List<&2, Pol.Command>) -> {Laws.replies(u, Pol.answer(ok, all, uu, es, left, ra)) == Laws.count.add(one_reply_per_request.pc_es(u, es), Laws.replies(u, ra)) : Laws.Count}:
  match es:
    case Nil{}:
      {==}
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, op}:
          %Equal.sym(Laws.Count, Laws.replies(u, Pol.answer(ok, all, uu, tail, Pol.after(left, req), ra)), Laws.count.add(one_reply_per_request.pc_es(u, tail), Laws.replies(u, ra)), one_reply_per_request.answer_count(u, ok, all, uu, tail, Pol.after(left, req), ra)) : {Laws.count.add(Laws.count.of(U32.is_eq(req, u)), _) == Laws.count.add(Laws.count.add(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, tail)), Laws.replies(u, ra)) : Laws.Count}
          %count.assoc(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, tail), Laws.replies(u, ra)) : {_ == Laws.count.add(Laws.count.add(Laws.count.of(U32.is_eq(req, u)), one_reply_per_request.pc_es(u, tail)), Laws.replies(u, ra)) : Laws.Count}
          {==}

def one_reply_per_request.part_in.put(hit: Bool, +bt: Pol.Batch, +u: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +uu: U32, +ra: List<&2, Pol.Command>, +pk: List<&2, Pol.Batch>, +pr: List<&2, Pol.Batch>, ih: {Laws.count.add(Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk)) == one_reply_per_request.pc_bs(u, pr) : Laws.Count}) -> {Laws.count.add(Laws.replies(u, Pol.answer_in.put(hit, bt, ok, outs, uu, ra)), one_reply_per_request.pc_bs(u, Pol.keep.put(hit, bt, pk))) == Laws.count.add(one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, +entries}:
          %Equal.sym(Laws.Count, Laws.replies(u, Pol.answer(ok, outs, uu, entries, outs, ra)), Laws.count.add(one_reply_per_request.pc_es(u, entries), Laws.replies(u, ra)), one_reply_per_request.answer_count(u, ok, outs, uu, entries, outs, ra)) : {Laws.count.add(_, one_reply_per_request.pc_bs(u, pk)) == Laws.count.add(one_reply_per_request.pc_es(u, entries), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}
          %Equal.sym(Laws.Count, Laws.count.add(Laws.count.add(one_reply_per_request.pc_es(u, entries), Laws.replies(u, ra)), one_reply_per_request.pc_bs(u, pk)), Laws.count.add(one_reply_per_request.pc_es(u, entries), Laws.count.add(Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk))), count.assoc(one_reply_per_request.pc_es(u, entries), Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk))) : {_ == Laws.count.add(one_reply_per_request.pc_es(u, entries), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}
          %Equal.sym(Laws.Count, Laws.count.add(Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk)), one_reply_per_request.pc_bs(u, pr), ih) : {Laws.count.add(one_reply_per_request.pc_es(u, entries), _) == Laws.count.add(one_reply_per_request.pc_es(u, entries), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}
          {==}
    case False{}:
      %Equal.sym(Laws.Count, Laws.count.add(Laws.replies(u, ra), Laws.count.add(one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, pk))), Laws.count.add(one_reply_per_request.pc_bt(u, bt), Laws.count.add(Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk))), count.swap(Laws.replies(u, ra), one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, pk))) : {_ == Laws.count.add(one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}
      %Equal.sym(Laws.Count, Laws.count.add(Laws.replies(u, ra), one_reply_per_request.pc_bs(u, pk)), one_reply_per_request.pc_bs(u, pr), ih) : {Laws.count.add(one_reply_per_request.pc_bt(u, bt), _) == Laws.count.add(one_reply_per_request.pc_bt(u, bt), one_reply_per_request.pc_bs(u, pr)) : Laws.Count}
      {==}

def one_reply_per_request.part_in(+u: U32, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +uu: U32, bs: List<&2, Pol.Batch>) -> {Laws.count.add(Laws.replies(u, Pol.answer_in(b, ok, outs, uu, bs)), one_reply_per_request.pc_bs(u, Pol.keep(b, bs))) == one_reply_per_request.pc_bs(u, bs) : Laws.Count}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      one_reply_per_request.part_in.put(U32.is_eq(Pol.id_of(bt), b), bt, u, ok, outs, uu, Pol.answer_in(b, ok, outs, uu, rest), Pol.keep(b, rest), rest, one_reply_per_request.part_in(u, b, ok, outs, uu, rest))

# One step.

def one_reply_per_request.put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32) -> {count.le(Laws.count.add(Laws.replies(u, [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), one_reply_per_request.pend_count(u, Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight))), Laws.count.add(one_reply_per_request.pend_count(u, Pol.State{next, reads, writes, inflight}), Laws.count.of(U32.is_eq(req, u)))) == True{} : Bool}:
  match k:
    case Pol.Read{}:
      match reads:
        case Pol.Batch{rid, rk, res}:
          count.or_run_r(U32.is_eq(req, u), one_reply_per_request.pc_es(u, res), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight))
    case Pol.Write{}:
      match writes:
        case Pol.Batch{wid, wk, wes}:
          count.or_run_w(U32.is_eq(req, u), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_es(u, wes), one_reply_per_request.pc_bs(u, inflight))

def one_reply_per_request.arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, +u: U32) -> {count.le(Laws.count.add(Laws.replies(u, Pol.arrived.commands(d, reads, writes, req, conn, gen)), one_reply_per_request.pend_count(u, Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen))), Laws.count.add(one_reply_per_request.pend_count(u, Pol.State{next, reads, writes, inflight}), Laws.count.of(U32.is_eq(req, u)))) == True{} : Bool}:
  match d:
    case Req.Reject{status, msg}:
      count.arr_reject(U32.is_eq(req, u), one_reply_per_request.pend_count(u, Pol.State{next, reads, writes, inflight}))
    case Req.Run{+op}:
      one_reply_per_request.put(Pol.kind_of(op), req, conn, gen, op, next, reads, writes, inflight, u)

def one_reply_per_request.flush(hr: Bool, hw: Bool, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(Laws.replies(u, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), one_reply_per_request.pend_count(u, Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))})), Laws.count.add(one_reply_per_request.pend_count(u, Pol.State{next, reads, writes, inflight}), Laws.Never{})) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          count.swap3_le(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bs(u, inflight))
        case False{}:
          count.swap3_le(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bs(u, inflight))
    case False{}:
      match hw:
        case True{}:
          count.same(Laws.count.add(one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight))))
        case False{}:
          count.same(Laws.count.add(one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight))))

def one_reply_per_request.settle(hr: Bool, hw: Bool, +u: U32, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +uu: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(Laws.replies(u, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, ok, outs, uu, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), one_reply_per_request.pend_count(u, Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))})), Laws.count.add(one_reply_per_request.pend_count(u, Pol.State{next, reads, writes, inflight}), Laws.Never{})) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          count.or_tt(Laws.replies(u, Pol.answer_in(b, ok, outs, uu, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), one_reply_per_request.pc_bs(u, Pol.keep(b, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight), one_reply_per_request.part_in(u, b, ok, outs, uu, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))
        case False{}:
          count.or_tf(Laws.replies(u, Pol.answer_in(b, ok, outs, uu, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), one_reply_per_request.pc_bs(u, Pol.keep(b, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight), one_reply_per_request.part_in(u, b, ok, outs, uu, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))
    case False{}:
      match hw:
        case True{}:
          count.or_ft(Laws.replies(u, Pol.answer_in(b, ok, outs, uu, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), one_reply_per_request.pc_bs(u, Pol.keep(b, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight), one_reply_per_request.part_in(u, b, ok, outs, uu, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))
        case False{}:
          count.or_ff(Laws.replies(u, Pol.answer_in(b, ok, outs, uu, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), one_reply_per_request.pc_bs(u, Pol.keep(b, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), one_reply_per_request.pc_bt(u, reads), one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight), one_reply_per_request.part_in(u, b, ok, outs, uu, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))

def one_reply_per_request.step(+u: U32, +s: Pol.State, +e: Pol.HostEvent) -> {count.le(one_reply_per_request.step_count(u, Pol.step(s, e)), Laws.count.add(one_reply_per_request.pend_count(u, s), Laws.arrival_of(u, e))) == True{} : Bool}:
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, r}:
          one_reply_per_request.arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, u)
        case Pol.Flushed{+b}:
          one_reply_per_request.flush(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), u, next, reads, writes, inflight)
        case Pol.Synced{+b, +ok, +outs, +uu}:
          one_reply_per_request.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), u, b, ok, outs, uu, next, reads, writes, inflight)
        case Pol.Died{+b}:
          one_reply_per_request.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), u, b, False{}, Nil{}, 0, next, reads, writes, inflight)
        case Pol.Gone{+cg, +gg}:
          count.gone(Laws.count.add(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, reads)), Laws.count.add(one_reply_per_request.pc_bt(u, Pol.drop_batch(cg, gg, writes)), one_reply_per_request.pc_bs(u, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(one_reply_per_request.pc_bt(u, reads), Laws.count.add(one_reply_per_request.pc_bt(u, writes), one_reply_per_request.pc_bs(u, inflight))), one_reply_per_request.pc_dropped(u, cg, gg, reads, writes, inflight))

def one_reply_per_request.reassoc(+x: Laws.Count, +p: Laws.Count, +a: Laws.Count, +y: Laws.Count, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(x, p), Laws.count.add(a, y))) == True{} : Bool}) -> {Laws.at_most_once(Laws.count.add(Laws.count.add(x, Laws.count.add(p, a)), y)) == True{} : Bool}:
  %count.assoc(x, p, a) : {Laws.at_most_once(Laws.count.add(_, y)) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(Laws.count.add(Laws.count.add(x, p), a), y), Laws.count.add(Laws.count.add(x, p), Laws.count.add(a, y)), count.assoc(Laws.count.add(x, p), a, y)) : {Laws.at_most_once(_) == True{} : Bool}
  h

def one_reply_per_request.comb(+u: U32, t1: Pol.State & List<&2, Pol.Command>, +x: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +p: Laws.Count, +a: Laws.Count, hle: {count.le(one_reply_per_request.step_count(u, t1), Laws.count.add(p, a)) == True{} : Bool}, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(Laws.replies(u, x), p), Laws.count.add(a, Laws.arrivals(u, rest)))) == True{} : Bool}) -> {one_reply_per_request.inv(u, t1, x, rest) == True{} : Bool}:
  match t1:
    case (+s1, +c1):
      %Equal.sym(Laws.Count, Laws.replies(u, List.append(&2, Pol.Command, x, c1)), Laws.count.add(Laws.replies(u, x), Laws.replies(u, c1)), one_reply_per_request.replies_append(u, x, c1)) : {Laws.at_most_once(Laws.count.add(Laws.count.add(_, one_reply_per_request.pend_count(u, s1)), Laws.arrivals(u, rest))) == True{} : Bool}
      %Equal.sym(Laws.Count, Laws.count.add(Laws.count.add(Laws.replies(u, x), Laws.replies(u, c1)), one_reply_per_request.pend_count(u, s1)), Laws.count.add(Laws.replies(u, x), Laws.count.add(Laws.replies(u, c1), one_reply_per_request.pend_count(u, s1))), count.assoc(Laws.replies(u, x), Laws.replies(u, c1), one_reply_per_request.pend_count(u, s1))) : {Laws.at_most_once(Laws.count.add(_, Laws.arrivals(u, rest))) == True{} : Bool}
      count.mono(Laws.replies(u, x), Laws.count.add(Laws.replies(u, c1), one_reply_per_request.pend_count(u, s1)), Laws.count.add(p, a), Laws.arrivals(u, rest), hle, one_reply_per_request.reassoc(Laws.replies(u, x), p, a, Laws.arrivals(u, rest), h))

def one_reply_per_request.go(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +u: U32, h: {one_reply_per_request.inv(u, t, acc, es) == True{} : Bool}) -> {Laws.at_most_once(Laws.replies(u, Pol.commands(Pol.run.go(es, t, acc)))) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (+s, +c):
          count.base(Laws.replies(u, List.append(&2, Pol.Command, acc, c)), one_reply_per_request.pend_count(u, s), h)
    case e <> rest:
      match t:
        case (+s, +c):
          one_reply_per_request.go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), u, one_reply_per_request.comb(u, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), rest, one_reply_per_request.pend_count(u, s), Laws.arrival_of(u, e), one_reply_per_request.step(u, s, e), h))

def Laws.one_reply_per_request(es, u, h):
  one_reply_per_request.go(es, (Pol.state0(), Nil{}), Nil{}, u, h)

# failed_batch_no_success
# -----------------------
# Fix a batch b and a request u that the commands of pre queue in b. The
# proof has three parts.
#
# go, over pre: the Queues for u in b, plus the entries for u in a batch
# with another id (open or in flight), plus the Arrived events for u in the
# rest of pre and in post, is at most Once. A step turns an arrival for u
# into a Queue in b or into an entry in a batch with another id, never
# both, and only moves or removes entries
# (step_count(step(s, e)) <= pend(s) + arrival_of(e)). The hypothesis of the
# law is the invariant at the start.
#
# mid, at the end of pre: the Queue makes the first summand Once, so no
# entry for u is in a batch with another id, and u does not arrive in post.
# The failure answers every entry of the batches with id b with
# respond(op, Unknown{}, up), which is not a success (no_success_on_unknown,
# S02), keep removes those batches, and a fresh batch is empty, so no entry
# for u is left.
#
# after, over post: with no entry for u in any batch and no arrival for u
# to come, no step emits a success to u (invariant ainv).

def failed_batch_no_success.qb(+u: U32, +b: U32, cs: List<&2, Pol.Command>) -> Laws.Count:
  match cs:
    case Nil{}:
      Laws.Never{}
    case c <> rest:
      Laws.count.add(Laws.count.of(Laws.queues(b, u, c)), failed_batch_no_success.qb(u, b, rest))

def failed_batch_no_success.pnb_es(+u: U32, +x: Bool, es: List<&2, Pol.Pending>) -> Laws.Count:
  match es:
    case Nil{}:
      Laws.Never{}
    case p <> rest:
      match p:
        case Pol.Pending{req, conn, gen, op}:
          Laws.count.add(Laws.count.of(Bool.and(U32.is_eq(req, u), x)), failed_batch_no_success.pnb_es(u, x, rest))

def failed_batch_no_success.pnb_bt(+u: U32, +b: U32, bt: Pol.Batch) -> Laws.Count:
  match bt:
    case Pol.Batch{id, kind, entries}:
      failed_batch_no_success.pnb_es(u, Bool.not(U32.is_eq(id, b)), entries)

def failed_batch_no_success.pnb_bs(+u: U32, +b: U32, bs: List<&2, Pol.Batch>) -> Laws.Count:
  match bs:
    case Nil{}:
      Laws.Never{}
    case bt <> rest:
      Laws.count.add(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, rest))

def failed_batch_no_success.pend(+u: U32, +b: U32, s: Pol.State) -> Laws.Count:
  match s:
    case Pol.State{next, reads, writes, inflight}:
      Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)))

def failed_batch_no_success.step_count(+u: U32, +b: U32, t: Pol.State & List<&2, Pol.Command>) -> Laws.Count:
  match t:
    case (s, c):
      Laws.count.add(failed_batch_no_success.qb(u, b, c), failed_batch_no_success.pend(u, b, s))

def failed_batch_no_success.inv(+u: U32, +b: U32, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>, es: List<&2, Pol.HostEvent>, post: List<&2, Pol.HostEvent>) -> Bool:
  match t:
    case (s, c):
      Laws.at_most_once(Laws.count.add(Laws.count.add(failed_batch_no_success.qb(u, b, List.append(&2, Pol.Command, acc, c)), failed_batch_no_success.pend(u, b, s)), Laws.arrivals(u, List.append(&2, Pol.HostEvent, es, post))))

def failed_batch_no_success.end(+u: U32, +b: U32, r: Pol.State & List<&2, Pol.Command>, post: List<&2, Pol.HostEvent>) -> Bool:
  match r:
    case (s, cs):
      Laws.at_most_once(Laws.count.add(Laws.count.add(failed_batch_no_success.qb(u, b, cs), failed_batch_no_success.pend(u, b, s)), Laws.arrivals(u, post)))

def failed_batch_no_success.qb_append(+u: U32, +b: U32, +xs: List<&2, Pol.Command>, +ys: List<&2, Pol.Command>) -> {failed_batch_no_success.qb(u, b, List.append(&2, Pol.Command, xs, ys)) == Laws.count.add(failed_batch_no_success.qb(u, b, xs), failed_batch_no_success.qb(u, b, ys)) : Laws.Count}:
  match xs:
    case Nil{}:
      {==}
    case x <> rest:
      %Equal.sym(Laws.Count, failed_batch_no_success.qb(u, b, List.append(&2, Pol.Command, rest, ys)), Laws.count.add(failed_batch_no_success.qb(u, b, rest), failed_batch_no_success.qb(u, b, ys)), failed_batch_no_success.qb_append(u, b, rest, ys)) : {Laws.count.add(Laws.count.of(Laws.queues(b, u, x)), _) == Laws.count.add(Laws.count.add(Laws.count.of(Laws.queues(b, u, x)), failed_batch_no_success.qb(u, b, rest)), failed_batch_no_success.qb(u, b, ys)) : Laws.Count}
      %count.assoc(Laws.count.of(Laws.queues(b, u, x)), failed_batch_no_success.qb(u, b, rest), failed_batch_no_success.qb(u, b, ys)) : {_ == Laws.count.add(Laws.count.add(Laws.count.of(Laws.queues(b, u, x)), failed_batch_no_success.qb(u, b, rest)), failed_batch_no_success.qb(u, b, ys)) : Laws.Count}
      {==}

def failed_batch_no_success.queued_imp(+u: U32, +b: U32, +cs: List<&2, Pol.Command>) -> {Laws.implies(Laws.queued_in(b, u, cs), count.le(Laws.Once{}, failed_batch_no_success.qb(u, b, cs))) == True{} : Bool}:
  match cs:
    case Nil{}:
      {==}
    case c <> rest:
      count.queued_step(Laws.queues(b, u, c), Laws.queued_in(b, u, rest), failed_batch_no_success.qb(u, b, rest), failed_batch_no_success.queued_imp(u, b, rest))


# The Queues an answer emits are none; drop and keep only remove entries.

def failed_batch_no_success.qb_answer(+u: U32, +b: U32, +ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +rest: List<&2, Pol.Command>, ih: {failed_batch_no_success.qb(u, b, rest) == Laws.Never{} : Laws.Count}) -> {failed_batch_no_success.qb(u, b, Pol.answer(ok, all, up, es, left, rest)) == Laws.Never{} : Laws.Count}:
  match es:
    case Nil{}:
      ih
    case p <> tail:
      match p:
        case Pol.Pending{+req, conn, gen, op}:
          failed_batch_no_success.qb_answer(u, b, ok, all, up, tail, Pol.after(left, req), rest, ih)

def failed_batch_no_success.qb_answer_in.put(hit: Bool, bt: Pol.Batch, +u: U32, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +rest: List<&2, Pol.Command>, ih: {failed_batch_no_success.qb(u, b, rest) == Laws.Never{} : Laws.Count}) -> {failed_batch_no_success.qb(u, b, Pol.answer_in.put(hit, bt, ok, outs, up, rest)) == Laws.Never{} : Laws.Count}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          failed_batch_no_success.qb_answer(u, b, ok, outs, up, entries, outs, rest, ih)
    case False{}:
      ih

def failed_batch_no_success.qb_answer_in(+u: U32, +b: U32, +bb: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, bs: List<&2, Pol.Batch>) -> {failed_batch_no_success.qb(u, b, Pol.answer_in(bb, ok, outs, up, bs)) == Laws.Never{} : Laws.Count}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.qb_answer_in.put(U32.is_eq(Pol.id_of(bt), bb), bt, u, b, ok, outs, up, Pol.answer_in(bb, ok, outs, up, rest), failed_batch_no_success.qb_answer_in(u, b, bb, ok, outs, up, rest))

def failed_batch_no_success.pnb_drop.put(hit: Bool, +u: U32, +x: Bool, +req: U32, +conn: U32, +gen: U32, -op: Req.Op, +pd: List<&2, Pol.Pending>, +pr: List<&2, Pol.Pending>, ih: {count.le(failed_batch_no_success.pnb_es(u, x, pd), failed_batch_no_success.pnb_es(u, x, pr)) == True{} : Bool}) -> {count.le(failed_batch_no_success.pnb_es(u, x, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, pd)), Laws.count.add(Laws.count.of(Bool.and(U32.is_eq(req, u), x)), failed_batch_no_success.pnb_es(u, x, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(Laws.count.of(Bool.and(U32.is_eq(req, u), x)), failed_batch_no_success.pnb_es(u, x, pd), failed_batch_no_success.pnb_es(u, x, pr), ih)
    case False{}:
      count.both(Laws.count.of(Bool.and(U32.is_eq(req, u), x)), failed_batch_no_success.pnb_es(u, x, pd), failed_batch_no_success.pnb_es(u, x, pr), ih)

def failed_batch_no_success.pnb_drop(+u: U32, +x: Bool, +cg: U32, +gg: U32, es: List<&2, Pol.Pending>) -> {count.le(failed_batch_no_success.pnb_es(u, x, Pol.drop(cg, gg, es)), failed_batch_no_success.pnb_es(u, x, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          failed_batch_no_success.pnb_drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), u, x, req, conn, gen, op, Pol.drop(cg, gg, rest), rest, failed_batch_no_success.pnb_drop(u, x, cg, gg, rest))

def failed_batch_no_success.pnb_drop_bt(+u: U32, +b: U32, +cg: U32, +gg: U32, bt: Pol.Batch) -> {count.le(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, bt)), failed_batch_no_success.pnb_bt(u, b, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{+id, kind, entries}:
      failed_batch_no_success.pnb_drop(u, Bool.not(U32.is_eq(id, b)), cg, gg, entries)

def failed_batch_no_success.pnb_drop_all(+u: U32, +b: U32, +cg: U32, +gg: U32, bs: List<&2, Pol.Batch>) -> {count.le(failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, bs)), failed_batch_no_success.pnb_bs(u, b, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      count.add_mono(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, bt)), failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, rest)), failed_batch_no_success.pnb_bs(u, b, rest), failed_batch_no_success.pnb_drop_bt(u, b, cg, gg, bt), failed_batch_no_success.pnb_drop_all(u, b, cg, gg, rest))

def failed_batch_no_success.pnb_keep.put(hit: Bool, +bt: Pol.Batch, +u: U32, +b: U32, +pk: List<&2, Pol.Batch>, +pr: List<&2, Pol.Batch>, ih: {count.le(failed_batch_no_success.pnb_bs(u, b, pk), failed_batch_no_success.pnb_bs(u, b, pr)) == True{} : Bool}) -> {count.le(failed_batch_no_success.pnb_bs(u, b, Pol.keep.put(hit, bt, pk)), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, pk), failed_batch_no_success.pnb_bs(u, b, pr), ih)
    case False{}:
      count.both(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, pk), failed_batch_no_success.pnb_bs(u, b, pr), ih)

def failed_batch_no_success.pnb_keep(+u: U32, +b: U32, +bb: U32, bs: List<&2, Pol.Batch>) -> {count.le(failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, bs)), failed_batch_no_success.pnb_bs(u, b, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.pnb_keep.put(U32.is_eq(Pol.id_of(bt), bb), bt, u, b, Pol.keep(bb, rest), rest, failed_batch_no_success.pnb_keep(u, b, bb, rest))

def failed_batch_no_success.pnb_dropped(+u: U32, +b: U32, +cg: U32, +gg: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, reads)), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)))) == True{} : Bool}:
  count.add_mono(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, reads)), failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, inflight))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), failed_batch_no_success.pnb_drop_bt(u, b, cg, gg, reads), count.add_mono(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, inflight)), failed_batch_no_success.pnb_bs(u, b, inflight), failed_batch_no_success.pnb_drop_bt(u, b, cg, gg, writes), failed_batch_no_success.pnb_drop_all(u, b, cg, gg, inflight)))

# One step of go.

def failed_batch_no_success.put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32, +b: U32) -> {count.le(Laws.count.add(failed_batch_no_success.qb(u, b, [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), failed_batch_no_success.pend(u, b, Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight))), Laws.count.add(failed_batch_no_success.pend(u, b, Pol.State{next, reads, writes, inflight}), Laws.count.of(U32.is_eq(req, u)))) == True{} : Bool}:
  match k:
    case Pol.Read{}:
      match reads:
        case Pol.Batch{+rid, rk, +res}:
          count.fb_run_r(U32.is_eq(rid, b), U32.is_eq(req, u), failed_batch_no_success.pnb_es(u, Bool.not(U32.is_eq(rid, b)), res), failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))
    case Pol.Write{}:
      match writes:
        case Pol.Batch{+wid, wk, +wes}:
          count.fb_run_w(U32.is_eq(wid, b), U32.is_eq(req, u), failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_es(u, Bool.not(U32.is_eq(wid, b)), wes), failed_batch_no_success.pnb_bs(u, b, inflight))

def failed_batch_no_success.arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, +u: U32, +b: U32) -> {count.le(Laws.count.add(failed_batch_no_success.qb(u, b, Pol.arrived.commands(d, reads, writes, req, conn, gen)), failed_batch_no_success.pend(u, b, Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen))), Laws.count.add(failed_batch_no_success.pend(u, b, Pol.State{next, reads, writes, inflight}), Laws.count.of(U32.is_eq(req, u)))) == True{} : Bool}:
  match d:
    case Req.Reject{status, msg}:
      count.le_add_r(U32.is_eq(req, u), failed_batch_no_success.pend(u, b, Pol.State{next, reads, writes, inflight}))
    case Req.Run{+op}:
      failed_batch_no_success.put(Pol.kind_of(op), req, conn, gen, op, next, reads, writes, inflight, u, b)

def failed_batch_no_success.flush(hr: Bool, hw: Bool, +u: U32, +b: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(failed_batch_no_success.qb(u, b, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), failed_batch_no_success.pend(u, b, Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))})), Laws.count.add(failed_batch_no_success.pend(u, b, Pol.State{next, reads, writes, inflight}), Laws.Never{})) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          count.swap3_le(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))
        case False{}:
          count.swap3_le(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))
    case False{}:
      match hw:
        case True{}:
          count.same(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))))
        case False{}:
          count.same(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))))

def failed_batch_no_success.settle(hr: Bool, hw: Bool, +u: U32, +b: U32, +bb: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(failed_batch_no_success.qb(u, b, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), failed_batch_no_success.pend(u, b, Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(bb, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))})), Laws.count.add(failed_batch_no_success.pend(u, b, Pol.State{next, reads, writes, inflight}), Laws.Never{})) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          count.fb_q(failed_batch_no_success.qb(u, b, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), failed_batch_no_success.qb_answer_in(u, b, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), count.le_trans(failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), failed_batch_no_success.pnb_keep(u, b, bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), count.swap3_le(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))))
        case False{}:
          count.fb_q(failed_batch_no_success.qb(u, b, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), failed_batch_no_success.qb_answer_in(u, b, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), count.le_trans(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), count.both(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight)), failed_batch_no_success.pnb_keep(u, b, bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), count.swap3_le(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, inflight))))
    case False{}:
      match hw:
        case True{}:
          count.fb_q(failed_batch_no_success.qb(u, b, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), failed_batch_no_success.qb_answer_in(u, b, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), count.le_trans(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), count.both(failed_batch_no_success.pnb_bt(u, b, reads), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), failed_batch_no_success.pnb_keep(u, b, bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), count.same(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))))))
        case False{}:
          count.fb_q(failed_batch_no_success.qb(u, b, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), failed_batch_no_success.qb_answer_in(u, b, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), count.le_trans(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.count.add(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}), count.both(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), count.both(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), failed_batch_no_success.pnb_bs(u, b, inflight), failed_batch_no_success.pnb_keep(u, b, bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), count.same(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))))))

def failed_batch_no_success.step(+u: U32, +b: U32, +s: Pol.State, +e: Pol.HostEvent) -> {count.le(failed_batch_no_success.step_count(u, b, Pol.step(s, e)), Laws.count.add(failed_batch_no_success.pend(u, b, s), Laws.arrival_of(u, e))) == True{} : Bool}:
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, r}:
          failed_batch_no_success.arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, u, b)
        case Pol.Flushed{+bb}:
          failed_batch_no_success.flush(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), u, b, next, reads, writes, inflight)
        case Pol.Synced{+bb, +ok, +outs, +up}:
          failed_batch_no_success.settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), u, b, bb, ok, outs, up, next, reads, writes, inflight)
        case Pol.Died{+bb}:
          failed_batch_no_success.settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), u, b, bb, False{}, Nil{}, 0, next, reads, writes, inflight)
        case Pol.Gone{+cg, +gg}:
          count.gone(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, reads)), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pnb_bs(u, b, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), failed_batch_no_success.pnb_dropped(u, b, cg, gg, reads, writes, inflight))

def failed_batch_no_success.reassoc(+x: Laws.Count, +p: Laws.Count, +a: Laws.Count, +y: Laws.Count, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(x, p), Laws.count.add(a, y))) == True{} : Bool}) -> {Laws.at_most_once(Laws.count.add(Laws.count.add(x, Laws.count.add(p, a)), y)) == True{} : Bool}:
  %count.assoc(x, p, a) : {Laws.at_most_once(Laws.count.add(_, y)) == True{} : Bool}
  %Equal.sym(Laws.Count, Laws.count.add(Laws.count.add(Laws.count.add(x, p), a), y), Laws.count.add(Laws.count.add(x, p), Laws.count.add(a, y)), count.assoc(Laws.count.add(x, p), a, y)) : {Laws.at_most_once(_) == True{} : Bool}
  h

def failed_batch_no_success.comb(+u: U32, +b: U32, t1: Pol.State & List<&2, Pol.Command>, +x: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +post: List<&2, Pol.HostEvent>, +p: Laws.Count, +a: Laws.Count, hle: {count.le(failed_batch_no_success.step_count(u, b, t1), Laws.count.add(p, a)) == True{} : Bool}, h: {Laws.at_most_once(Laws.count.add(Laws.count.add(failed_batch_no_success.qb(u, b, x), p), Laws.count.add(a, Laws.arrivals(u, List.append(&2, Pol.HostEvent, rest, post))))) == True{} : Bool}) -> {failed_batch_no_success.inv(u, b, t1, x, rest, post) == True{} : Bool}:
  match t1:
    case (+s1, +c1):
      %Equal.sym(Laws.Count, failed_batch_no_success.qb(u, b, List.append(&2, Pol.Command, x, c1)), Laws.count.add(failed_batch_no_success.qb(u, b, x), failed_batch_no_success.qb(u, b, c1)), failed_batch_no_success.qb_append(u, b, x, c1)) : {Laws.at_most_once(Laws.count.add(Laws.count.add(_, failed_batch_no_success.pend(u, b, s1)), Laws.arrivals(u, List.append(&2, Pol.HostEvent, rest, post)))) == True{} : Bool}
      %Equal.sym(Laws.Count, Laws.count.add(Laws.count.add(failed_batch_no_success.qb(u, b, x), failed_batch_no_success.qb(u, b, c1)), failed_batch_no_success.pend(u, b, s1)), Laws.count.add(failed_batch_no_success.qb(u, b, x), Laws.count.add(failed_batch_no_success.qb(u, b, c1), failed_batch_no_success.pend(u, b, s1))), count.assoc(failed_batch_no_success.qb(u, b, x), failed_batch_no_success.qb(u, b, c1), failed_batch_no_success.pend(u, b, s1))) : {Laws.at_most_once(Laws.count.add(_, Laws.arrivals(u, List.append(&2, Pol.HostEvent, rest, post)))) == True{} : Bool}
      count.mono(failed_batch_no_success.qb(u, b, x), Laws.count.add(failed_batch_no_success.qb(u, b, c1), failed_batch_no_success.pend(u, b, s1)), Laws.count.add(p, a), Laws.arrivals(u, List.append(&2, Pol.HostEvent, rest, post)), hle, failed_batch_no_success.reassoc(failed_batch_no_success.qb(u, b, x), p, a, Laws.arrivals(u, List.append(&2, Pol.HostEvent, rest, post)), h))

def failed_batch_no_success.go(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +post: List<&2, Pol.HostEvent>, +u: U32, +b: U32, h: {failed_batch_no_success.inv(u, b, t, acc, es, post) == True{} : Bool}) -> {failed_batch_no_success.end(u, b, Pol.run.go(es, t, acc), post) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (s, c):
          h
    case e <> rest:
      match t:
        case (+s, +c):
          failed_batch_no_success.go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), post, u, b, failed_batch_no_success.comb(u, b, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), rest, post, failed_batch_no_success.pend(u, b, s), Laws.arrival_of(u, e), failed_batch_no_success.step(u, b, s, e), h))

def failed_batch_no_success.pc_es(+u: U32, es: List<&2, Pol.Pending>) -> Laws.Count:
  match es:
    case Nil{}:
      Laws.Never{}
    case p <> rest:
      match p:
        case Pol.Pending{req, conn, gen, op}:
          Laws.count.add(Laws.count.of(U32.is_eq(req, u)), failed_batch_no_success.pc_es(u, rest))

def failed_batch_no_success.pc_bt(+u: U32, bt: Pol.Batch) -> Laws.Count:
  match bt:
    case Pol.Batch{id, kind, entries}:
      failed_batch_no_success.pc_es(u, entries)

def failed_batch_no_success.pc_bs(+u: U32, bs: List<&2, Pol.Batch>) -> Laws.Count:
  match bs:
    case Nil{}:
      Laws.Never{}
    case bt <> rest:
      Laws.count.add(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, rest))

def failed_batch_no_success.pc_drop.put(hit: Bool, +u: U32, +req: U32, +conn: U32, +gen: U32, -op: Req.Op, +pd: List<&2, Pol.Pending>, +pr: List<&2, Pol.Pending>, ih: {count.le(failed_batch_no_success.pc_es(u, pd), failed_batch_no_success.pc_es(u, pr)) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_es(u, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, pd)), Laws.count.add(Laws.count.of(U32.is_eq(req, u)), failed_batch_no_success.pc_es(u, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(Laws.count.of(U32.is_eq(req, u)), failed_batch_no_success.pc_es(u, pd), failed_batch_no_success.pc_es(u, pr), ih)
    case False{}:
      count.both(Laws.count.of(U32.is_eq(req, u)), failed_batch_no_success.pc_es(u, pd), failed_batch_no_success.pc_es(u, pr), ih)

def failed_batch_no_success.pc_drop(+u: U32, +cg: U32, +gg: U32, es: List<&2, Pol.Pending>) -> {count.le(failed_batch_no_success.pc_es(u, Pol.drop(cg, gg, es)), failed_batch_no_success.pc_es(u, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          failed_batch_no_success.pc_drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), u, req, conn, gen, op, Pol.drop(cg, gg, rest), rest, failed_batch_no_success.pc_drop(u, cg, gg, rest))

def failed_batch_no_success.pc_drop_bt(+u: U32, +cg: U32, +gg: U32, bt: Pol.Batch) -> {count.le(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, bt)), failed_batch_no_success.pc_bt(u, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      failed_batch_no_success.pc_drop(u, cg, gg, entries)

def failed_batch_no_success.pc_drop_all(+u: U32, +cg: U32, +gg: U32, bs: List<&2, Pol.Batch>) -> {count.le(failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, bs)), failed_batch_no_success.pc_bs(u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      count.add_mono(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, bt)), failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, rest)), failed_batch_no_success.pc_bs(u, rest), failed_batch_no_success.pc_drop_bt(u, cg, gg, bt), failed_batch_no_success.pc_drop_all(u, cg, gg, rest))

def failed_batch_no_success.pc_dropped(+u: U32, +cg: U32, +gg: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)))) == True{} : Bool}:
  count.add_mono(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, reads)), failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, inflight))), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), failed_batch_no_success.pc_drop_bt(u, cg, gg, reads), count.add_mono(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, inflight)), failed_batch_no_success.pc_bs(u, inflight), failed_batch_no_success.pc_drop_bt(u, cg, gg, writes), failed_batch_no_success.pc_drop_all(u, cg, gg, inflight)))

def failed_batch_no_success.pc_keep.put(hit: Bool, +bt: Pol.Batch, +u: U32, +pk: List<&2, Pol.Batch>, +pr: List<&2, Pol.Batch>, ih: {count.le(failed_batch_no_success.pc_bs(u, pk), failed_batch_no_success.pc_bs(u, pr)) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_bs(u, Pol.keep.put(hit, bt, pk)), Laws.count.add(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, pk), failed_batch_no_success.pc_bs(u, pr), ih)
    case False{}:
      count.both(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, pk), failed_batch_no_success.pc_bs(u, pr), ih)

def failed_batch_no_success.pc_keep(+u: U32, +b: U32, bs: List<&2, Pol.Batch>) -> {count.le(failed_batch_no_success.pc_bs(u, Pol.keep(b, bs)), failed_batch_no_success.pc_bs(u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.pc_keep.put(U32.is_eq(Pol.id_of(bt), b), bt, u, Pol.keep(b, rest), rest, failed_batch_no_success.pc_keep(u, b, rest))

# mid: the failure's replies.

def failed_batch_no_success.pc_of_pnb_t(+u: U32, es: List<&2, Pol.Pending>, +hp: {count.le(failed_batch_no_success.pnb_es(u, True{}, es), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_es(u, es), Laws.Never{}) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, conn, gen, op}:
          count.keep_none(U32.is_eq(req, u), failed_batch_no_success.pnb_es(u, True{}, rest), failed_batch_no_success.pc_es(u, rest), hp, failed_batch_no_success.pc_of_pnb_t(u, rest, count.never_tail(Bool.and(U32.is_eq(req, u), True{}), failed_batch_no_success.pnb_es(u, True{}, rest), hp)))

def failed_batch_no_success.pc_of_pnb(x: Bool, +u: U32, hx: {x == False{} : Bool}, es: List<&2, Pol.Pending>, hp: {count.le(failed_batch_no_success.pnb_es(u, Bool.not(x), es), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_es(u, es), Laws.Never{}) == True{} : Bool}:
  match x:
    case True{}:
      true_ne_false({count.le(failed_batch_no_success.pc_es(u, es), Laws.Never{}) == True{} : Bool}, hx)
    case False{}:
      failed_batch_no_success.pc_of_pnb_t(u, es, hp)

def failed_batch_no_success.pc_bt_none(bt: Pol.Batch, +u: U32, +b: U32, heq: {U32.is_eq(Pol.id_of(bt), b) == False{} : Bool}, hp: {count.le(failed_batch_no_success.pnb_bt(u, b, bt), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_bt(u, bt), Laws.Never{}) == True{} : Bool}:
  match bt:
    case Pol.Batch{+id, kind, entries}:
      failed_batch_no_success.pc_of_pnb(U32.is_eq(id, b), u, heq, entries, hp)

def failed_batch_no_success.fresh_none(hit: Bool, bt: Pol.Batch, +u: U32, +b: U32, -n: U32, -k: Pol.BatchKind, heq: {U32.is_eq(Pol.id_of(bt), b) == hit : Bool}, hp: {count.le(failed_batch_no_success.pnb_bt(u, b, bt), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_bt(u, Pol.fresh(hit, n, k, bt)), Laws.Never{}) == True{} : Bool}:
  match hit:
    case True{}:
      {==}
    case False{}:
      failed_batch_no_success.pc_bt_none(bt, u, b, heq, hp)

def failed_batch_no_success.park_never(hit: Bool, +u: U32, +b: U32, +bt: Pol.Batch, +bs: List<&2, Pol.Batch>, hb: {count.le(failed_batch_no_success.pnb_bt(u, b, bt), Laws.Never{}) == True{} : Bool}, hs: {count.le(failed_batch_no_success.pnb_bs(u, b, bs), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pnb_bs(u, b, Pol.park(hit, bt, bs)), Laws.Never{}) == True{} : Bool}:
  match hit:
    case True{}:
      count.never_add(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, bs), hb, hs)
    case False{}:
      hs

def failed_batch_no_success.keep_none.put(hit: Bool, +bt: Pol.Batch, +u: U32, +b: U32, heq: {U32.is_eq(Pol.id_of(bt), b) == hit : Bool}, +rest: List<&2, Pol.Batch>, hb: {count.le(failed_batch_no_success.pnb_bt(u, b, bt), Laws.Never{}) == True{} : Bool}, ih: {count.le(failed_batch_no_success.pc_bs(u, rest), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_bs(u, Pol.keep.put(hit, bt, rest)), Laws.Never{}) == True{} : Bool}:
  match hit:
    case True{}:
      ih
    case False{}:
      count.never_add(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, rest), failed_batch_no_success.pc_bt_none(bt, u, b, heq, hb), ih)

def failed_batch_no_success.keep_none(+u: U32, +b: U32, bs: List<&2, Pol.Batch>, +hp: {count.le(failed_batch_no_success.pnb_bs(u, b, bs), Laws.Never{}) == True{} : Bool}) -> {count.le(failed_batch_no_success.pc_bs(u, Pol.keep(b, bs)), Laws.Never{}) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.keep_none.put(U32.is_eq(Pol.id_of(bt), b), bt, u, b, {==}, Pol.keep(b, rest), count.never_l(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, rest), hp), failed_batch_no_success.keep_none(u, b, rest, count.never_r(failed_batch_no_success.pnb_bt(u, b, bt), failed_batch_no_success.pnb_bs(u, b, rest), hp)))

def failed_batch_no_success.answer_none(+all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +u: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, ih: {Laws.no_success_to(u, rest) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.answer(False{}, all, up, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, +op}:
          %Equal.sym(Bool, Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, up)), False{}, Laws.no_success_on_unknown(op, up)) : {Bool.and(Laws.implies(U32.is_eq(req, u), Bool.not(_)), Laws.no_success_to(u, Pol.answer(False{}, all, up, tail, Pol.after(left, req), rest))) == True{} : Bool}
          %Equal.sym(Bool, Laws.implies(U32.is_eq(req, u), True{}), True{}, implies_true_r(U32.is_eq(req, u))) : {Bool.and(_, Laws.no_success_to(u, Pol.answer(False{}, all, up, tail, Pol.after(left, req), rest))) == True{} : Bool}
          failed_batch_no_success.answer_none(all, up, u, tail, Pol.after(left, req), rest, ih)

def failed_batch_no_success.answer_none_in.put(hit: Bool, bt: Pol.Batch, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +u: U32, -rest: List<&2, Pol.Command>, ih: {Laws.no_success_to(u, rest) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.answer_in.put(hit, bt, False{}, outs, up, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          failed_batch_no_success.answer_none(outs, up, u, entries, outs, rest, ih)
    case False{}:
      ih

def failed_batch_no_success.answer_none_in(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +u: U32, bs: List<&2, Pol.Batch>) -> {Laws.no_success_to(u, Pol.answer_in(b, False{}, outs, up, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.answer_none_in.put(U32.is_eq(Pol.id_of(bt), b), bt, outs, up, u, Pol.answer_in(b, False{}, outs, up, rest), failed_batch_no_success.answer_none_in(b, outs, up, u, rest))

def failed_batch_no_success.nst_opened(h: Bool, -n: U32, -k: Pol.BatchKind, +u: U32, -rest: List<&2, Pol.Command>, ih: {Laws.no_success_to(u, rest) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.opened(h, n, k, rest)) == True{} : Bool}:
  match h:
    case True{}:
      ih
    case False{}:
      ih


def failed_batch_no_success.ainv(+u: U32, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>, es: List<&2, Pol.HostEvent>) -> Bool:
  match t:
    case (s, c):
      match s:
        case Pol.State{next, reads, writes, inflight}:
          Bool.and(Bool.and(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, es), Laws.Never{})), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)))

def failed_batch_no_success.q_app(+u: U32, +xs: List<&2, Pol.Command>, -ys: List<&2, Pol.Command>, +hx: {Laws.no_success_to(u, xs) == True{} : Bool}, hy: {Laws.no_success_to(u, ys) == True{} : Bool}) -> {Laws.no_success_to(u, List.append(&2, Pol.Command, xs, ys)) == True{} : Bool}:
  match xs:
    case Nil{}:
      hy
    case x <> rest:
      match x:
        case Pol.Send{+req, +conn, +gen, +reply}:
          and_intro(Laws.implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(reply))), Laws.no_success_to(u, List.append(&2, Pol.Command, rest, ys)), and_l(Laws.implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(reply))), Laws.no_success_to(u, rest), hx), failed_batch_no_success.q_app(u, rest, ys, and_r(Laws.implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(reply))), Laws.no_success_to(u, rest), hx), hy))
        case Pol.Queue{b, req, op}:
          failed_batch_no_success.q_app(u, rest, ys, hx, hy)
        case Pol.Open{b, k}:
          failed_batch_no_success.q_app(u, rest, ys, hx, hy)

def failed_batch_no_success.answer_quiet(+u: U32, +ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, +hp: {count.le(failed_batch_no_success.pc_es(u, es), Laws.Never{}) == True{} : Bool}, ih: {Laws.no_success_to(u, rest) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.answer(ok, all, up, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          and_intro(Laws.implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), up)))), Laws.no_success_to(u, Pol.answer(ok, all, up, tail, Pol.after(left, req), rest)), count.implies_never(U32.is_eq(req, u), Bool.not(Rsp.is_success(Rsp.respond(op, Pol.outcome(ok, all, left, req), up))), failed_batch_no_success.pc_es(u, tail), hp), failed_batch_no_success.answer_quiet(u, ok, all, up, tail, Pol.after(left, req), rest, count.never_tail(U32.is_eq(req, u), failed_batch_no_success.pc_es(u, tail), hp), ih))

def failed_batch_no_success.answer_quiet_in.put(hit: Bool, bt: Pol.Batch, +u: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, -rest: List<&2, Pol.Command>, hb: {count.le(failed_batch_no_success.pc_bt(u, bt), Laws.Never{}) == True{} : Bool}, ih: {Laws.no_success_to(u, rest) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.answer_in.put(hit, bt, ok, outs, up, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          failed_batch_no_success.answer_quiet(u, ok, outs, up, entries, outs, rest, hb, ih)
    case False{}:
      ih

def failed_batch_no_success.answer_quiet_in(+u: U32, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, bs: List<&2, Pol.Batch>, +hp: {count.le(failed_batch_no_success.pc_bs(u, bs), Laws.Never{}) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.answer_in(b, ok, outs, up, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      failed_batch_no_success.answer_quiet_in.put(U32.is_eq(Pol.id_of(bt), b), bt, u, ok, outs, up, Pol.answer_in(b, ok, outs, up, rest), count.never_l(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, rest), hp), failed_batch_no_success.answer_quiet_in(u, b, ok, outs, up, rest, count.never_r(failed_batch_no_success.pc_bt(u, bt), failed_batch_no_success.pc_bs(u, rest), hp)))


def failed_batch_no_success.a_put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}) == True{} : Bool}, hx: {U32.is_eq(req, u) == False{} : Bool}, ha: {count.le(Laws.arrivals(u, rest), Laws.Never{}) == True{} : Bool}, hn: {Laws.no_success_to(u, xs) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), xs, rest) == True{} : Bool}:
  match k:
    case Pol.Read{}:
      match reads:
        case Pol.Batch{rid, rk, +res}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.Batch{rid, rk, Pol.Pending{req, conn, gen, op} <> res}), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, [Pol.Queue{rid, req, op}])), count.push_none_r(U32.is_eq(req, u), failed_batch_no_success.pc_es(u, res), failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), hx, hp), ha, failed_batch_no_success.q_app(u, xs, [Pol.Queue{rid, req, op}], hn, {==}))
    case Pol.Write{}:
      match writes:
        case Pol.Batch{wid, wk, +wes}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.Batch{wid, wk, Pol.Pending{req, conn, gen, op} <> wes}), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, [Pol.Queue{wid, req, op}])), count.push_none_w(U32.is_eq(req, u), failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_es(u, wes), failed_batch_no_success.pc_bs(u, inflight), hx, hp), ha, failed_batch_no_success.q_app(u, xs, [Pol.Queue{wid, req, op}], hn, {==}))

def failed_batch_no_success.a_arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, +u: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}) == True{} : Bool}, +hx: {U32.is_eq(req, u) == False{} : Bool}, ha: {count.le(Laws.arrivals(u, rest), Laws.Never{}) == True{} : Bool}, hn: {Laws.no_success_to(u, xs) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), xs, rest) == True{} : Bool}:
  match d:
    case Req.Reject{+status, +msg}:
      and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}])), hp, ha, failed_batch_no_success.q_app(u, xs, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}], hn, and_intro(Laws.implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(Rsp.Error{status, msg}))), True{}, implies_false(U32.is_eq(req, u), Bool.not(Rsp.is_success(Rsp.Error{status, msg})), hx), {==})))
    case Req.Run{+op}:
      failed_batch_no_success.a_put(Pol.kind_of(op), req, conn, gen, op, next, reads, writes, inflight, u, xs, rest, hp, hx, ha, hn)


def failed_batch_no_success.a_flush(hr: Bool, hw: Bool, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hp: {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}) == True{} : Bool}, ha: {count.le(Laws.arrivals(u, rest), Laws.Never{}) == True{} : Bool}, hn: {Laws.no_success_to(u, xs) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), xs, rest) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, Pol.bump(True{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})))), count.swap3_never(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), hp), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})), hn, {==}))
        case False{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, Pol.bump(True{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})))), count.swap3_never(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), hp), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})), hn, {==}))
    case False{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, Pol.bump(False{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})))), hp, ha, failed_batch_no_success.q_app(u, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})), hn, {==}))
        case False{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, Pol.bump(False{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})))), hp, ha, failed_batch_no_success.q_app(u, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})), hn, {==}))

def failed_batch_no_success.a_settle(hr: Bool, hw: Bool, +bb: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hp: {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}) == True{} : Bool}, ha: {count.le(Laws.arrivals(u, rest), Laws.Never{}) == True{} : Bool}, hn: {Laws.no_success_to(u, xs) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(bb, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), xs, rest) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, Pol.bump(True{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))))), count.trans_never(failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), failed_batch_no_success.pc_bs(u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), failed_batch_no_success.pc_keep(u, bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), count.swap3_never(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), hp)), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))), hn, failed_batch_no_success.answer_quiet_in(u, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)), count.swap3_never(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), hp))))
        case False{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, Pol.bump(True{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))))), count.never_add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), count.never_l(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp)), count.trans_never(failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), failed_batch_no_success.pc_bs(u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), failed_batch_no_success.pc_keep(u, bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), count.never_add(failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), count.never_l(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp), count.never_r(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp))))), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), hn, failed_batch_no_success.answer_quiet_in(u, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)), count.never_add(failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, inflight), count.never_l(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp), count.never_r(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp))))))
    case False{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(True{}, Pol.bump(False{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))))), count.never_add(failed_batch_no_success.pc_bt(u, reads), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), count.never_l(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp), count.trans_never(failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), failed_batch_no_success.pc_bs(u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), failed_batch_no_success.pc_keep(u, bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp))), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), hn, failed_batch_no_success.answer_quiet_in(u, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp))))
        case False{}:
          and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(False{}, Pol.bump(False{}, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))))), count.never_add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), count.never_l(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp), count.never_add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), count.never_l(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp)), count.trans_never(failed_batch_no_success.pc_bs(u, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), failed_batch_no_success.pc_bs(u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), failed_batch_no_success.pc_keep(u, bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), count.never_r(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp))))), ha, failed_batch_no_success.q_app(u, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), hn, failed_batch_no_success.answer_quiet_in(u, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)), count.never_r(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight), count.never_r(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight)), hp)))))

def failed_batch_no_success.a_gone(+cg: U32, +gg: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +u: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}) == True{} : Bool}, ha: {count.le(Laws.arrivals(u, rest), Laws.Never{}) == True{} : Bool}, hn: {Laws.no_success_to(u, xs) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.State{next, Pol.drop_batch(cg, gg, reads), Pol.drop_batch(cg, gg, writes), Pol.drop_all(cg, gg, inflight)}, Nil{}), xs, rest) == True{} : Bool}:
  and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, inflight)))), Laws.Never{}), count.le(Laws.arrivals(u, rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, xs, Nil{})), count.trans_never(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.drop_batch(cg, gg, writes)), failed_batch_no_success.pc_bs(u, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), failed_batch_no_success.pc_dropped(u, cg, gg, reads, writes, inflight), hp), ha, failed_batch_no_success.q_app(u, xs, Nil{}, hn, {==}))

def failed_batch_no_success.astep(+u: U32, +s: Pol.State, +c: List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +e: Pol.HostEvent, +rest: List<&2, Pol.HostEvent>, +h: {failed_batch_no_success.ainv(u, (s, c), acc, e <> rest) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), rest) == True{} : Bool}:
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, +r}:
          failed_batch_no_success.a_arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, u, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Arrived{req, conn, gen, r} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), not_true(U32.is_eq(req, u), count.not_of(U32.is_eq(req, u), Laws.arrivals(u, rest), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Arrived{req, conn, gen, r} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))), count.never_tail(U32.is_eq(req, u), Laws.arrivals(u, rest), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Arrived{req, conn, gen, r} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h)), and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Arrived{req, conn, gen, r} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Flushed{+bb}:
          failed_batch_no_success.a_flush(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), next, reads, writes, inflight, u, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Flushed{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Flushed{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Flushed{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Synced{+bb, +ok, +outs, +up}:
          failed_batch_no_success.a_settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), bb, ok, outs, up, next, reads, writes, inflight, u, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Synced{bb, ok, outs, up} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Synced{bb, ok, outs, up} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Synced{bb, ok, outs, up} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Died{+bb}:
          failed_batch_no_success.a_settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), bb, False{}, Nil{}, 0, next, reads, writes, inflight, u, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Died{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Died{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Died{bb} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Gone{+cg, +gg}:
          failed_batch_no_success.a_gone(cg, gg, next, reads, writes, inflight, u, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Gone{cg, gg} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Gone{cg, gg} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Pol.Gone{cg, gg} <> rest), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h))

def failed_batch_no_success.after(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +u: U32, h: {failed_batch_no_success.ainv(u, t, acc, es) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.commands(Pol.run.go(es, t, acc))) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (s, +c):
          match s:
            case Pol.State{next, reads, writes, inflight}:
              and3_3(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, reads), Laws.count.add(failed_batch_no_success.pc_bt(u, writes), failed_batch_no_success.pc_bs(u, inflight))), Laws.Never{}), count.le(Laws.arrivals(u, Nil{}), Laws.Never{}), Laws.no_success_to(u, List.append(&2, Pol.Command, acc, c)), h)
    case e <> rest:
      match t:
        case (+s, +c):
          failed_batch_no_success.after(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), u, failed_batch_no_success.astep(u, s, c, acc, e, rest, h))

def failed_batch_no_success.fail(+hr: Bool, +hw: Bool, +u: U32, +b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +post: List<&2, Pol.HostEvent>, heqr: {U32.is_eq(Pol.id_of(reads), b) == hr : Bool}, heqw: {U32.is_eq(Pol.id_of(writes), b) == hw : Bool}, +hP: {count.le(Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.Never{}) == True{} : Bool}, hA: {count.le(Laws.arrivals(u, post), Laws.Never{}) == True{} : Bool}) -> {failed_batch_no_success.ainv(u, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), Nil{}, post) == True{} : Bool}:
  and3_intro(count.le(Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(hr, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), Laws.Never{}), count.le(Laws.arrivals(u, post), Laws.Never{}), Laws.no_success_to(u, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), count.never_add(failed_batch_no_success.pc_bt(u, Pol.fresh(hr, next, Pol.Read{}, reads)), Laws.count.add(failed_batch_no_success.pc_bt(u, Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight))))), failed_batch_no_success.fresh_none(hr, reads, u, b, next, Pol.Read{}, heqr, count.never_l(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), hP)), count.never_add(failed_batch_no_success.pc_bt(u, Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes)), failed_batch_no_success.pc_bs(u, Pol.keep(b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), failed_batch_no_success.fresh_none(hw, writes, u, b, Pol.bump(hr, next), Pol.Write{}, heqw, count.never_l(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight), count.never_r(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), hP))), failed_batch_no_success.keep_none(u, b, Pol.park(hw, writes, Pol.park(hr, reads, inflight)), failed_batch_no_success.park_never(hw, u, b, writes, Pol.park(hr, reads, inflight), count.never_l(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight), count.never_r(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), hP)), failed_batch_no_success.park_never(hr, u, b, reads, inflight, count.never_l(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), hP), count.never_r(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight), count.never_r(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight)), hP))))))), hA, failed_batch_no_success.nst_opened(hr, next, Pol.Read{}, u, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, False{}, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))), failed_batch_no_success.nst_opened(hw, Pol.bump(hr, next), Pol.Write{}, u, Pol.answer_in(b, False{}, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight))), failed_batch_no_success.answer_none_in(b, outs, up, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight))))))

def failed_batch_no_success.mid(r: Pol.State & List<&2, Pol.Command>, +post: List<&2, Pol.HostEvent>, +b: U32, +u: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +hE: {failed_batch_no_success.end(u, b, r, post) == True{} : Bool}, +hq: {Laws.queued_in(b, u, Pol.commands(r)) == True{} : Bool}) -> {Laws.no_success_to(u, Pol.commands(Pol.run(Laws.final(r), Pol.Synced{b, False{}, outs, up} <> post))) == True{} : Bool} & {Laws.no_success_to(u, Pol.commands(Pol.run(Laws.final(r), Pol.Died{b} <> post))) == True{} : Bool}:
  match r:
    case (s, +cs):
      match s:
        case Pol.State{+next, +reads, +writes, +inflight}:
          (failed_batch_no_success.after(post, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Synced{b, False{}, outs, up}), List.append(&2, Pol.Command, Nil{}, Nil{}), u, failed_batch_no_success.fail(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), u, b, outs, up, next, reads, writes, inflight, post, {==}, {==}, count.pinch_p(failed_batch_no_success.qb(u, b, cs), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.arrivals(u, post), hE, implies_mp(Laws.queued_in(b, u, cs), count.le(Laws.Once{}, failed_batch_no_success.qb(u, b, cs)), failed_batch_no_success.queued_imp(u, b, cs), hq)), count.pinch_a(failed_batch_no_success.qb(u, b, cs), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.arrivals(u, post), hE, implies_mp(Laws.queued_in(b, u, cs), count.le(Laws.Once{}, failed_batch_no_success.qb(u, b, cs)), failed_batch_no_success.queued_imp(u, b, cs), hq)))), failed_batch_no_success.after(post, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Died{b}), List.append(&2, Pol.Command, Nil{}, Nil{}), u, failed_batch_no_success.fail(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), u, b, Nil{}, 0, next, reads, writes, inflight, post, {==}, {==}, count.pinch_p(failed_batch_no_success.qb(u, b, cs), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.arrivals(u, post), hE, implies_mp(Laws.queued_in(b, u, cs), count.le(Laws.Once{}, failed_batch_no_success.qb(u, b, cs)), failed_batch_no_success.queued_imp(u, b, cs), hq)), count.pinch_a(failed_batch_no_success.qb(u, b, cs), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, reads), Laws.count.add(failed_batch_no_success.pnb_bt(u, b, writes), failed_batch_no_success.pnb_bs(u, b, inflight))), Laws.arrivals(u, post), hE, implies_mp(Laws.queued_in(b, u, cs), count.le(Laws.Once{}, failed_batch_no_success.qb(u, b, cs)), failed_batch_no_success.queued_imp(u, b, cs), hq)))))

def Laws.failed_batch_no_success(pre, post, b, u, outs, up, h, q):
  failed_batch_no_success.mid(Pol.run(Pol.state0(), pre), post, b, u, outs, up, failed_batch_no_success.go(pre, (Pol.state0(), Nil{}), Nil{}, post, u, b, h), q)

# gone_no_reply
# -------------
# Fix a connection ck and a generation gk. The Gone{ck, gk} drops every
# entry on (ck, gk) from the open batches and the batches in flight
# (drop_none: drop.put keeps an entry exactly when gone_from is False{}, and
# gone_from is the entry's own (ck, gk) test). After it no entry on
# (ck, gk) is left and, by the hypothesis, none arrives in post, so no step
# emits a Send to (ck, gk) (invariant ainv, the same after phase as
# failed_batch_no_success with "on (ck, gk)" for "for u").

def gone_no_reply.gc_es(+ck: U32, +gk: U32, es: List<&2, Pol.Pending>) -> Laws.Count:
  match es:
    case Nil{}:
      Laws.Never{}
    case p <> rest:
      match p:
        case Pol.Pending{req, conn, gen, op}:
          Laws.count.add(Laws.count.of(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), gone_no_reply.gc_es(ck, gk, rest))

def gone_no_reply.gc_bt(+ck: U32, +gk: U32, bt: Pol.Batch) -> Laws.Count:
  match bt:
    case Pol.Batch{id, kind, entries}:
      gone_no_reply.gc_es(ck, gk, entries)

def gone_no_reply.gc_bs(+ck: U32, +gk: U32, bs: List<&2, Pol.Batch>) -> Laws.Count:
  match bs:
    case Nil{}:
      Laws.Never{}
    case bt <> rest:
      Laws.count.add(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, rest))

def gone_no_reply.gc_drop.put(hit: Bool, +ck: U32, +gk: U32, +req: U32, +conn: U32, +gen: U32, -op: Req.Op, +pd: List<&2, Pol.Pending>, +pr: List<&2, Pol.Pending>, ih: {count.le(gone_no_reply.gc_es(ck, gk, pd), gone_no_reply.gc_es(ck, gk, pr)) == True{} : Bool}) -> {count.le(gone_no_reply.gc_es(ck, gk, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, pd)), Laws.count.add(Laws.count.of(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), gone_no_reply.gc_es(ck, gk, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(Laws.count.of(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), gone_no_reply.gc_es(ck, gk, pd), gone_no_reply.gc_es(ck, gk, pr), ih)
    case False{}:
      count.both(Laws.count.of(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), gone_no_reply.gc_es(ck, gk, pd), gone_no_reply.gc_es(ck, gk, pr), ih)

def gone_no_reply.gc_drop(+ck: U32, +gk: U32, +cg: U32, +gg: U32, es: List<&2, Pol.Pending>) -> {count.le(gone_no_reply.gc_es(ck, gk, Pol.drop(cg, gg, es)), gone_no_reply.gc_es(ck, gk, es)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          gone_no_reply.gc_drop.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, cg, gg), ck, gk, req, conn, gen, op, Pol.drop(cg, gg, rest), rest, gone_no_reply.gc_drop(ck, gk, cg, gg, rest))

def gone_no_reply.gc_drop_bt(+ck: U32, +gk: U32, +cg: U32, +gg: U32, bt: Pol.Batch) -> {count.le(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, bt)), gone_no_reply.gc_bt(ck, gk, bt)) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      gone_no_reply.gc_drop(ck, gk, cg, gg, entries)

def gone_no_reply.gc_drop_all(+ck: U32, +gk: U32, +cg: U32, +gg: U32, bs: List<&2, Pol.Batch>) -> {count.le(gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, bs)), gone_no_reply.gc_bs(ck, gk, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      count.add_mono(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, bt)), gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, rest)), gone_no_reply.gc_bs(ck, gk, rest), gone_no_reply.gc_drop_bt(ck, gk, cg, gg, bt), gone_no_reply.gc_drop_all(ck, gk, cg, gg, rest))

def gone_no_reply.gc_dropped(+ck: U32, +gk: U32, +cg: U32, +gg: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)))) == True{} : Bool}:
  count.add_mono(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, reads)), gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, inflight))), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), gone_no_reply.gc_drop_bt(ck, gk, cg, gg, reads), count.add_mono(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, writes)), gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, inflight)), gone_no_reply.gc_bs(ck, gk, inflight), gone_no_reply.gc_drop_bt(ck, gk, cg, gg, writes), gone_no_reply.gc_drop_all(ck, gk, cg, gg, inflight)))

def gone_no_reply.gc_keep.put(hit: Bool, +bt: Pol.Batch, +ck: U32, +gk: U32, +pk: List<&2, Pol.Batch>, +pr: List<&2, Pol.Batch>, ih: {count.le(gone_no_reply.gc_bs(ck, gk, pk), gone_no_reply.gc_bs(ck, gk, pr)) == True{} : Bool}) -> {count.le(gone_no_reply.gc_bs(ck, gk, Pol.keep.put(hit, bt, pk)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, pr))) == True{} : Bool}:
  match hit:
    case True{}:
      count.grow(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, pk), gone_no_reply.gc_bs(ck, gk, pr), ih)
    case False{}:
      count.both(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, pk), gone_no_reply.gc_bs(ck, gk, pr), ih)

def gone_no_reply.gc_keep(+ck: U32, +gk: U32, +b: U32, bs: List<&2, Pol.Batch>) -> {count.le(gone_no_reply.gc_bs(ck, gk, Pol.keep(b, bs)), gone_no_reply.gc_bs(ck, gk, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      gone_no_reply.gc_keep.put(U32.is_eq(Pol.id_of(bt), b), bt, ck, gk, Pol.keep(b, rest), rest, gone_no_reply.gc_keep(ck, gk, b, rest))


def gone_no_reply.ainv(+ck: U32, +gk: U32, t: Pol.State & List<&2, Pol.Command>, acc: List<&2, Pol.Command>, es: List<&2, Pol.HostEvent>) -> Bool:
  match t:
    case (s, c):
      match s:
        case Pol.State{next, reads, writes, inflight}:
          Bool.and(Bool.and(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, es))), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)))

def gone_no_reply.q_app(+ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, -ys: List<&2, Pol.Command>, +hx: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}, hy: {Laws.no_send_to(ck, gk, ys) == True{} : Bool}) -> {Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, ys)) == True{} : Bool}:
  match xs:
    case Nil{}:
      hy
    case x <> rest:
      match x:
        case Pol.Send{+req, +conn, +gen, +reply}:
          and_intro(Bool.not(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, rest, ys)), and_l(Bool.not(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), Laws.no_send_to(ck, gk, rest), hx), gone_no_reply.q_app(ck, gk, rest, ys, and_r(Bool.not(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), Laws.no_send_to(ck, gk, rest), hx), hy))
        case Pol.Queue{b, req, op}:
          gone_no_reply.q_app(ck, gk, rest, ys, hx, hy)
        case Pol.Open{b, k}:
          gone_no_reply.q_app(ck, gk, rest, ys, hx, hy)

def gone_no_reply.answer_quiet(+ck: U32, +gk: U32, +ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -rest: List<&2, Pol.Command>, +hp: {count.le(gone_no_reply.gc_es(ck, gk, es), Laws.Never{}) == True{} : Bool}, ih: {Laws.no_send_to(ck, gk, rest) == True{} : Bool}) -> {Laws.no_send_to(ck, gk, Pol.answer(ok, all, up, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      ih
    case p <> +tail:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          and_intro(Bool.not(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), Laws.no_send_to(ck, gk, Pol.answer(ok, all, up, tail, Pol.after(left, req), rest)), count.not_of(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), gone_no_reply.gc_es(ck, gk, tail), hp), gone_no_reply.answer_quiet(ck, gk, ok, all, up, tail, Pol.after(left, req), rest, count.never_tail(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), gone_no_reply.gc_es(ck, gk, tail), hp), ih))

def gone_no_reply.answer_quiet_in.put(hit: Bool, bt: Pol.Batch, +ck: U32, +gk: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, -rest: List<&2, Pol.Command>, hb: {count.le(gone_no_reply.gc_bt(ck, gk, bt), Laws.Never{}) == True{} : Bool}, ih: {Laws.no_send_to(ck, gk, rest) == True{} : Bool}) -> {Laws.no_send_to(ck, gk, Pol.answer_in.put(hit, bt, ok, outs, up, rest)) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          gone_no_reply.answer_quiet(ck, gk, ok, outs, up, entries, outs, rest, hb, ih)
    case False{}:
      ih

def gone_no_reply.answer_quiet_in(+ck: U32, +gk: U32, +b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, bs: List<&2, Pol.Batch>, +hp: {count.le(gone_no_reply.gc_bs(ck, gk, bs), Laws.Never{}) == True{} : Bool}) -> {Laws.no_send_to(ck, gk, Pol.answer_in(b, ok, outs, up, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      gone_no_reply.answer_quiet_in.put(U32.is_eq(Pol.id_of(bt), b), bt, ck, gk, ok, outs, up, Pol.answer_in(b, ok, outs, up, rest), count.never_l(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, rest), hp), gone_no_reply.answer_quiet_in(ck, gk, b, ok, outs, up, rest, count.never_r(gone_no_reply.gc_bt(ck, gk, bt), gone_no_reply.gc_bs(ck, gk, rest), hp)))


def gone_no_reply.a_put(k: Pol.BatchKind, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}) == True{} : Bool}, hx: {Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)) == False{} : Bool}, ha: {Bool.not(Laws.arrives_on(ck, gk, rest)) == True{} : Bool}, hn: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, (Pol.arrived.put(k, Pol.Pending{req, conn, gen, op}, next, reads, writes, inflight), [Pol.Queue{Pol.open_of(k, reads, writes), req, op}]), xs, rest) == True{} : Bool}:
  match k:
    case Pol.Read{}:
      match reads:
        case Pol.Batch{rid, rk, +res}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.Batch{rid, rk, Pol.Pending{req, conn, gen, op} <> res}), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, [Pol.Queue{rid, req, op}])), count.push_none_r(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), gone_no_reply.gc_es(ck, gk, res), gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), hx, hp), ha, gone_no_reply.q_app(ck, gk, xs, [Pol.Queue{rid, req, op}], hn, {==}))
    case Pol.Write{}:
      match writes:
        case Pol.Batch{wid, wk, +wes}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.Batch{wid, wk, Pol.Pending{req, conn, gen, op} <> wes}), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, [Pol.Queue{wid, req, op}])), count.push_none_w(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_es(ck, gk, wes), gone_no_reply.gc_bs(ck, gk, inflight), hx, hp), ha, gone_no_reply.q_app(ck, gk, xs, [Pol.Queue{wid, req, op}], hn, {==}))

def gone_no_reply.a_arrived(d: Req.Decision, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +req: U32, +conn: U32, +gen: U32, +ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}) == True{} : Bool}, +hx: {Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)) == False{} : Bool}, ha: {Bool.not(Laws.arrives_on(ck, gk, rest)) == True{} : Bool}, hn: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, (Pol.arrived.state(d, next, reads, writes, inflight, req, conn, gen), Pol.arrived.commands(d, reads, writes, req, conn, gen)), xs, rest) == True{} : Bool}:
  match d:
    case Req.Reject{+status, +msg}:
      and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}])), hp, ha, gone_no_reply.q_app(ck, gk, xs, [Pol.Send{req, conn, gen, Rsp.Error{status, msg}}], hn, and_intro(Bool.not(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk))), True{}, not_false(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), hx), {==})))
    case Req.Run{+op}:
      gone_no_reply.a_put(Pol.kind_of(op), req, conn, gen, op, next, reads, writes, inflight, ck, gk, xs, rest, hp, hx, ha, hn)


def gone_no_reply.a_flush(hr: Bool, hw: Bool, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hp: {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}) == True{} : Bool}, ha: {Bool.not(Laws.arrives_on(ck, gk, rest)) == True{} : Bool}, hn: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.park(hw, writes, Pol.park(hr, reads, inflight))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Nil{}))), xs, rest) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, Pol.bump(True{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})))), count.swap3_never(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), hp), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})), hn, {==}))
        case False{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, Pol.bump(True{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})))), count.swap3_never(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), hp), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Nil{})), hn, {==}))
    case False{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, Pol.bump(False{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})))), hp, ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})), hn, {==}))
        case False{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, Pol.bump(False{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})))), hp, ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Nil{})), hn, {==}))

def gone_no_reply.a_settle(hr: Bool, hw: Bool, +bb: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +up: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, +hp: {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}) == True{} : Bool}, ha: {Bool.not(Laws.arrives_on(ck, gk, rest)) == True{} : Bool}, hn: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, (Pol.State{Pol.bump(hw, Pol.bump(hr, next)), Pol.fresh(hr, next, Pol.Read{}, reads), Pol.fresh(hw, Pol.bump(hr, next), Pol.Write{}, writes), Pol.keep(bb, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))}, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))), xs, rest) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, Pol.bump(True{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))))), count.trans_never(gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), gone_no_reply.gc_bs(ck, gk, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), gone_no_reply.gc_keep(ck, gk, bb, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), count.swap3_never(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), hp)), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))))), hn, gone_no_reply.answer_quiet_in(ck, gk, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)), count.swap3_never(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), hp))))
        case False{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, Pol.bump(True{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))))), count.never_add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), count.never_l(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp)), count.trans_never(gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))), gone_no_reply.gc_bs(ck, gk, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), gone_no_reply.gc_keep(ck, gk, bb, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), count.never_add(gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), count.never_l(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp), count.never_r(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp))))), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), hn, gone_no_reply.answer_quiet_in(ck, gk, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)), count.never_add(gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, inflight), count.never_l(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp), count.never_r(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp))))))
    case False{}:
      match hw:
        case True{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(True{}, Pol.bump(False{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))))), count.never_add(gone_no_reply.gc_bt(ck, gk, reads), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), count.never_l(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp), count.trans_never(gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))), gone_no_reply.gc_bs(ck, gk, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), gone_no_reply.gc_keep(ck, gk, bb, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp))), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), hn, gone_no_reply.answer_quiet_in(ck, gk, bb, ok, outs, up, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp))))
        case False{}:
          and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, next, Pol.Read{}, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.fresh(False{}, Pol.bump(False{}, next), Pol.Write{}, writes)), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))))), count.never_add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), count.never_l(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp), count.never_add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), count.never_l(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp)), count.trans_never(gone_no_reply.gc_bs(ck, gk, Pol.keep(bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), gone_no_reply.gc_bs(ck, gk, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), gone_no_reply.gc_keep(ck, gk, bb, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), count.never_r(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp))))), ha, gone_no_reply.q_app(ck, gk, xs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))))), hn, gone_no_reply.answer_quiet_in(ck, gk, bb, ok, outs, up, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)), count.never_r(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight), count.never_r(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight)), hp)))))

def gone_no_reply.a_gone(+cg: U32, +gg: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, +ck: U32, +gk: U32, +xs: List<&2, Pol.Command>, +rest: List<&2, Pol.HostEvent>, hp: {count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}) == True{} : Bool}, ha: {Bool.not(Laws.arrives_on(ck, gk, rest)) == True{} : Bool}, hn: {Laws.no_send_to(ck, gk, xs) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, (Pol.State{next, Pol.drop_batch(cg, gg, reads), Pol.drop_batch(cg, gg, writes), Pol.drop_all(cg, gg, inflight)}, Nil{}), xs, rest) == True{} : Bool}:
  and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, inflight)))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, xs, Nil{})), count.trans_never(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(cg, gg, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(cg, gg, inflight)))), Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), gone_no_reply.gc_dropped(ck, gk, cg, gg, reads, writes, inflight), hp), ha, gone_no_reply.q_app(ck, gk, xs, Nil{}, hn, {==}))

def gone_no_reply.astep(+ck: U32, +gk: U32, +s: Pol.State, +c: List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +e: Pol.HostEvent, +rest: List<&2, Pol.HostEvent>, +h: {gone_no_reply.ainv(ck, gk, (s, c), acc, e <> rest) == True{} : Bool}) -> {gone_no_reply.ainv(ck, gk, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), rest) == True{} : Bool}:
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      match e:
        case Pol.Arrived{+req, +conn, +gen, +r}:
          gone_no_reply.a_arrived(Req.classify(r), next, reads, writes, inflight, req, conn, gen, ck, gk, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Arrived{req, conn, gen, r} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), or_false_l(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), Laws.arrives_on(ck, gk, rest), not_true(Bool.or(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), Laws.arrives_on(ck, gk, rest)), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Arrived{req, conn, gen, r} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))), not_false(Laws.arrives_on(ck, gk, rest), or_false_r(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), Laws.arrives_on(ck, gk, rest), not_true(Bool.or(Bool.and(U32.is_eq(conn, ck), U32.is_eq(gen, gk)), Laws.arrives_on(ck, gk, rest)), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Arrived{req, conn, gen, r} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h)))), and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Arrived{req, conn, gen, r} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Flushed{+bb}:
          gone_no_reply.a_flush(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), next, reads, writes, inflight, ck, gk, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Flushed{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Flushed{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Flushed{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Synced{+bb, +ok, +outs, +up}:
          gone_no_reply.a_settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), bb, ok, outs, up, next, reads, writes, inflight, ck, gk, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Synced{bb, ok, outs, up} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Synced{bb, ok, outs, up} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Synced{bb, ok, outs, up} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Died{+bb}:
          gone_no_reply.a_settle(U32.is_eq(Pol.id_of(reads), bb), U32.is_eq(Pol.id_of(writes), bb), bb, False{}, Nil{}, 0, next, reads, writes, inflight, ck, gk, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Died{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Died{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Died{bb} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))
        case Pol.Gone{+cg, +gg}:
          gone_no_reply.a_gone(cg, gg, next, reads, writes, inflight, ck, gk, List.append(&2, Pol.Command, acc, c), rest, and3_1(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Gone{cg, gg} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_2(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Gone{cg, gg} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h), and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Pol.Gone{cg, gg} <> rest)), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h))

def gone_no_reply.after(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>, +ck: U32, +gk: U32, h: {gone_no_reply.ainv(ck, gk, t, acc, es) == True{} : Bool}) -> {Laws.no_send_to(ck, gk, Pol.commands(Pol.run.go(es, t, acc))) == True{} : Bool}:
  match es:
    case Nil{}:
      match t:
        case (s, +c):
          match s:
            case Pol.State{next, reads, writes, inflight}:
              and3_3(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, reads), Laws.count.add(gone_no_reply.gc_bt(ck, gk, writes), gone_no_reply.gc_bs(ck, gk, inflight))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, Nil{})), Laws.no_send_to(ck, gk, List.append(&2, Pol.Command, acc, c)), h)
    case e <> rest:
      match t:
        case (+s, +c):
          gone_no_reply.after(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c), ck, gk, gone_no_reply.astep(ck, gk, s, c, acc, e, rest, h))

def gone_no_reply.drop_none.put(hit: Bool, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +ck: U32, +gk: U32, heq: {Pol.gone_from(Pol.Pending{req, conn, gen, op}, ck, gk) == hit : Bool}, +rest: List<&2, Pol.Pending>, ih: {count.le(gone_no_reply.gc_es(ck, gk, rest), Laws.Never{}) == True{} : Bool}) -> {count.le(gone_no_reply.gc_es(ck, gk, Pol.drop.put(hit, Pol.Pending{req, conn, gen, op}, rest)), Laws.Never{}) == True{} : Bool}:
  match hit:
    case True{}:
      ih
    case False{}:
      %Equal.sym(Bool, Pol.gone_from(Pol.Pending{req, conn, gen, op}, ck, gk), False{}, heq) : {count.le(Laws.count.add(Laws.count.of(_), gone_no_reply.gc_es(ck, gk, rest)), Laws.Never{}) == True{} : Bool}
      ih

def gone_no_reply.drop_none(+ck: U32, +gk: U32, es: List<&2, Pol.Pending>) -> {count.le(gone_no_reply.gc_es(ck, gk, Pol.drop(ck, gk, es)), Laws.Never{}) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +rest:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          gone_no_reply.drop_none.put(Pol.gone_from(Pol.Pending{req, conn, gen, op}, ck, gk), req, conn, gen, op, ck, gk, {==}, Pol.drop(ck, gk, rest), gone_no_reply.drop_none(ck, gk, rest))

def gone_no_reply.drop_none_bt(+ck: U32, +gk: U32, bt: Pol.Batch) -> {count.le(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, bt)), Laws.Never{}) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      gone_no_reply.drop_none(ck, gk, entries)

def gone_no_reply.drop_none_all(+ck: U32, +gk: U32, bs: List<&2, Pol.Batch>) -> {count.le(gone_no_reply.gc_bs(ck, gk, Pol.drop_all(ck, gk, bs)), Laws.Never{}) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      count.never_add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, bt)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(ck, gk, rest)), gone_no_reply.drop_none_bt(ck, gk, bt), gone_no_reply.drop_none_all(ck, gk, rest))

def gone_no_reply.mid(r: Pol.State & List<&2, Pol.Command>, +post: List<&2, Pol.HostEvent>, +ck: U32, +gk: U32, h: {Laws.arrives_on(ck, gk, post) == False{} : Bool}) -> {Laws.no_send_to(ck, gk, Pol.commands(Pol.run(Laws.final(r), Pol.Gone{ck, gk} <> post))) == True{} : Bool}:
  match r:
    case (s, cs):
      match s:
        case Pol.State{+next, +reads, +writes, +inflight}:
          gone_no_reply.after(post, Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Gone{ck, gk}), List.append(&2, Pol.Command, Nil{}, Nil{}), ck, gk, and3_intro(count.le(Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(ck, gk, inflight)))), Laws.Never{}), Bool.not(Laws.arrives_on(ck, gk, post)), True{}, count.never_add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, reads)), Laws.count.add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(ck, gk, inflight))), gone_no_reply.drop_none_bt(ck, gk, reads), count.never_add(gone_no_reply.gc_bt(ck, gk, Pol.drop_batch(ck, gk, writes)), gone_no_reply.gc_bs(ck, gk, Pol.drop_all(ck, gk, inflight)), gone_no_reply.drop_none_bt(ck, gk, writes), gone_no_reply.drop_none_all(ck, gk, inflight))), not_false(Laws.arrives_on(ck, gk, post), h), {==}))

def Laws.gone_no_reply(pre, post, c, g, h):
  gone_no_reply.mid(Pol.run(Pol.state0(), pre), post, c, g, h)

# post_exact, like_exact and post_query_exact
# --------------------------------------------
# Each law names its route, with the id inside it, as constructors, and
# names the auth and the body as far as classify reads them on that route:
# post_exact names all three, like_exact leaves the body free (no LikePost
# row reads it), and post_query_exact leaves the auth and the body free
# (the PostById row reads neither). So classify computes to one Run, as
# for auth_before_id: no match.

def Laws.post_exact(uid, name, s):
  {==}

def Laws.like_exact(uid, name, id, body):
  {==}

def Laws.post_query_exact(id, auth, body):
  {==}

# committed_write_succeeds
# ------------------------
# step on Synced{b, True{}, outs, u} emits an Open for each open batch with
# id b, then answer_in(b, True{}, outs, u, bs): for each entry of each batch
# in bs with id b, Send{req, conn, gen, respond(op, head(left, outs, req),
# u)}, where left is what the entries before it have left of outs. Let
# (r, o) be req's only pair in outs, and let o commit op. If left's first
# pair has the id req, it is a pair of outs (AllIn, kept by after) with
# that id, so it is (r, o) (mem_commits); else head falls back to
# find(outs, req), which returns o (find). respond(op, o, u) is then a
# success (success). Every Send after an entry's own, and every Open, only
# adds to the commands that Send is in (ss_cons, ca_cons, bt_cons,
# ba_cons, ba_answer). The batches the law names, those of reads, writes
# and inflight with id b, are the batches settle answers (park), in
# another order (settle).

def committed_write_succeeds.both(-G: Type, +x: Bool, ht: {x == True{} : Bool}, hf: {x == False{} : Bool}) -> G:
  match x:
    case True{}:
      true_ne_false(G, hf)
    case False{}:
      false_ne_true(G, ht)

def committed_write_succeeds.not_both(-G: Type, +x: Bool, ht: {x == True{} : Bool}, hn: {Bool.not(x) == True{} : Bool}) -> G:
  match x:
    case True{}:
      false_ne_true(G, hn)
    case False{}:
      false_ne_true(G, ht)

def committed_write_succeeds.or_f(+a: Bool, h: {Bool.or(a, False{}) == True{} : Bool}) -> {a == True{} : Bool}:
  match a:
    case True{}:
      {==}
    case False{}:
      false_ne_true({False{} == True{} : Bool}, h)

def committed_write_succeeds.pc(+op: Req.Op, p: +Sigma<U32, _ => Rsp.Outcome>) -> Bool:
  match p:
    case (r, o):
      Laws.commits(op, o)

def committed_write_succeeds.pid(p: +Sigma<U32, _ => Rsp.Outcome>) -> U32:
  match p:
    case (r, o):
      r

def committed_write_succeeds.same_id(-r2: U32, -o2: Rsp.Outcome, -r: U32, -o: Rsp.Outcome, -req: U32, e: {(r2, o2) == (r, o) : +Sigma<U32, _ => Rsp.Outcome>}, heq: {U32.is_eq(r, req) == True{} : Bool}) -> {U32.is_eq(r2, req) == True{} : Bool}:
  %Equal.sym(+Sigma<U32, _ => Rsp.Outcome>, (r2, o2), (r, o), e) : {U32.is_eq(committed_write_succeeds.pid(_), req) == True{} : Bool}
  heq

def committed_write_succeeds.mem_has(all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -r: U32, -o: Rsp.Outcome, +req: U32, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), heq: {U32.is_eq(r, req) == True{} : Bool}) -> {Laws.has_outcome(all, req) == True{} : Bool}:
  match all:
    case Nil{}:
      match m:
    case (+r2, o2) <> +t:
      match m:
        case Inl{e}:
          %Equal.sym(+Sigma<U32, _ => Rsp.Outcome>, (r2, o2), (r, o), e) : {Laws.has_outcome(_ <> t, req) == True{} : Bool}
          %Equal.sym(Bool, U32.is_eq(r, req), True{}, heq) : {Bool.or(_, Laws.has_outcome(t, req)) == True{} : Bool}
          {==}
        case Inr{n}:
          %Equal.sym(Bool, Laws.has_outcome(t, req), True{}, committed_write_succeeds.mem_has(t, r, o, req, n, heq)) : {Bool.or(U32.is_eq(r2, req), _) == True{} : Bool}
          or_true_r(U32.is_eq(r2, req))

# The pair (r, o) of all with id req is the only one, so it is the pair
# that commits op. At the head pair (r2, o2): if it is (r, o), its id is
# req and h says o2 commits op (mc_head); if (r, o) is further on, t has a
# pair with id req, so r2 is not req and h is the same claim about t
# (mc_rest).
def committed_write_succeeds.mc_head(hit: Bool, +r2: U32, +o2: Rsp.Outcome, +t: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -r: U32, -o: Rsp.Outcome, +req: U32, +op: Req.Op, heq2: {U32.is_eq(r2, req) == hit : Bool}, e: {(r2, o2) == (r, o) : +Sigma<U32, _ => Rsp.Outcome>}, heq: {U32.is_eq(r, req) == True{} : Bool}, h: {Bool.or(Bool.and(hit, Bool.and(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req)))), Bool.and(Bool.not(hit), Laws.only_commit(t, req, op))) == True{} : Bool}) -> {Laws.commits(op, o) == True{} : Bool}:
  match hit:
    case True{}:
      %e : {committed_write_succeeds.pc(op, _) == True{} : Bool}
      and_l(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req)), committed_write_succeeds.or_f(Bool.and(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req))), h))
    case False{}:
      committed_write_succeeds.both({Laws.commits(op, o) == True{} : Bool}, U32.is_eq(r2, req), committed_write_succeeds.same_id(r2, o2, r, o, req, e, heq), heq2)

def committed_write_succeeds.mc_rest(hit: Bool, -r2: U32, +o2: Rsp.Outcome, +t: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, heq2: {U32.is_eq(r2, req) == hit : Bool}, ht: {Laws.has_outcome(t, req) == True{} : Bool}, h: {Bool.or(Bool.and(hit, Bool.and(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req)))), Bool.and(Bool.not(hit), Laws.only_commit(t, req, op))) == True{} : Bool}) -> {Laws.only_commit(t, req, op) == True{} : Bool}:
  match hit:
    case True{}:
      committed_write_succeeds.not_both({Laws.only_commit(t, req, op) == True{} : Bool}, Laws.has_outcome(t, req), ht, and_r(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req)), committed_write_succeeds.or_f(Bool.and(Laws.commits(op, o2), Bool.not(Laws.has_outcome(t, req))), h)))
    case False{}:
      h

def committed_write_succeeds.mem_commits(all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -r: U32, -o: Rsp.Outcome, +req: U32, +op: Req.Op, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), +heq: {U32.is_eq(r, req) == True{} : Bool}, h: {Laws.only_commit(all, req, op) == True{} : Bool}) -> {Laws.commits(op, o) == True{} : Bool}:
  match all:
    case Nil{}:
      match m:
    case (+r2, +o2) <> +t:
      match m:
        case Inl{e}:
          committed_write_succeeds.mc_head(U32.is_eq(r2, req), r2, o2, t, r, o, req, op, {==}, e, heq, h)
        case Inr{+n}:
          committed_write_succeeds.mem_commits(t, r, o, req, op, n, heq, committed_write_succeeds.mc_rest(U32.is_eq(r2, req), r2, o2, t, req, op, {==}, committed_write_succeeds.mem_has(t, r, o, req, n, heq), h))

# find returns the only pair's outcome. Stated as an implication, so that
# the recursive call needs no premise.
def committed_write_succeeds.imp_c(+c: Bool, +n: Bool) -> {Laws.implies(Bool.or(Bool.and(c, n), False{}), c) == True{} : Bool}:
  match c:
    case True{}:
      implies_true_r(Bool.or(Bool.and(True{}, n), False{}))
    case False{}:
      {==}

def committed_write_succeeds.find.pick(hit: Bool, +o: Rsp.Outcome, +rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, ih: {Laws.implies(Laws.only_commit(rest, req, op), Laws.commits(op, Pol.find(rest, req))) == True{} : Bool}) -> {Laws.implies(Bool.or(Bool.and(hit, Bool.and(Laws.commits(op, o), Bool.not(Laws.has_outcome(rest, req)))), Bool.and(Bool.not(hit), Laws.only_commit(rest, req, op))), Laws.commits(op, Pol.find.pick(hit, o, Pol.find(rest, req)))) == True{} : Bool}:
  match hit:
    case True{}:
      committed_write_succeeds.imp_c(Laws.commits(op, o), Bool.not(Laws.has_outcome(rest, req)))
    case False{}:
      ih

def committed_write_succeeds.find(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op) -> {Laws.implies(Laws.only_commit(outs, req, op), Laws.commits(op, Pol.find(outs, req))) == True{} : Bool}:
  match outs:
    case Nil{}:
      {==}
    case (+r, o) <> +rest:
      committed_write_succeeds.find.pick(U32.is_eq(r, req), o, rest, req, op, committed_write_succeeds.find(rest, req, op))

def committed_write_succeeds.find_mp(+outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, h: {Laws.only_commit(outs, req, op) == True{} : Bool}) -> {Laws.commits(op, Pol.find(outs, req)) == True{} : Bool}:
  implies_mp(Laws.only_commit(outs, req, op), Laws.commits(op, Pol.find(outs, req)), committed_write_succeeds.find(outs, req, op), h)

def committed_write_succeeds.head.pick(hit: Bool, -r: U32, -o: Rsp.Outcome, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, heq: {U32.is_eq(r, req) == hit : Bool}, m: Mem(+Sigma<U32, _ => Rsp.Outcome>, (r, o), all), h: {Laws.only_commit(all, req, op) == True{} : Bool}) -> {Laws.commits(op, Pol.head.pick(hit, o, all, req)) == True{} : Bool}:
  match hit:
    case True{}:
      committed_write_succeeds.mem_commits(all, r, o, req, op, m, heq, h)
    case False{}:
      committed_write_succeeds.find_mp(all, req, op, h)

def committed_write_succeeds.head(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all), h: {Laws.only_commit(all, req, op) == True{} : Bool}) -> {Laws.commits(op, Pol.head(left, all, req)) == True{} : Bool}:
  match left:
    case Nil{}:
      committed_write_succeeds.find_mp(all, req, op, h)
    case (+r, o) <> rest:
      match hl:
        case (m, hr):
          committed_write_succeeds.head.pick(U32.is_eq(r, req), r, o, all, req, op, {==}, m, h)

# A write whose outcome commits it is answered with a success: Created
# (201) or LikedReply (201, or 200 when already liked).
def committed_write_succeeds.Goal(op: Req.Op, o: Rsp.Outcome, u: U32) -> Type:
  {Rsp.is_success(Rsp.respond(op, o, u)) == True{} : Bool}

def committed_write_succeeds.success(op: Req.Op, o: Rsp.Outcome, -u: U32, h: {Laws.commits(op, o) == True{} : Bool}) -> committed_write_succeeds.Goal(op, o, u):
  match op:
    case Req.SelectOne{}:
      false_ne_true(committed_write_succeeds.Goal(Req.SelectOne{}, o, u), h)
    case Req.FeedQuery{}:
      false_ne_true(committed_write_succeeds.Goal(Req.FeedQuery{}, o, u), h)
    case Req.PostQuery{pid}:
      false_ne_true(committed_write_succeeds.Goal(Req.PostQuery{pid}, o, u), h)
    case Req.InsertPost{uid, name, text}:
      match o:
        case Rsp.Committed{id, ts}:
          {==}
        case Rsp.Liked{already}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Liked{already}, u), h)
        case Rsp.Post{post}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Post{post}, u), h)
        case Rsp.Posts{posts}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Posts{posts}, u), h)
        case Rsp.Ping{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Ping{}, u), h)
        case Rsp.NotFound{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.NotFound{}, u), h)
        case Rsp.Unknown{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Unknown{}, u), h)
        case Rsp.Failed{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertPost{uid, name, text}, Rsp.Failed{}, u), h)
    case Req.InsertLike{uid, pid}:
      match o:
        case Rsp.Committed{id, ts}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Committed{id, ts}, u), h)
        case Rsp.Liked{already}:
          match already:
            case True{}:
              {==}
            case False{}:
              {==}
        case Rsp.Post{post}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Post{post}, u), h)
        case Rsp.Posts{posts}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Posts{posts}, u), h)
        case Rsp.Ping{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Ping{}, u), h)
        case Rsp.NotFound{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.NotFound{}, u), h)
        case Rsp.Unknown{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Unknown{}, u), h)
        case Rsp.Failed{}:
          false_ne_true(committed_write_succeeds.Goal(Req.InsertLike{uid, pid}, Rsp.Failed{}, u), h)

def committed_write_succeeds.skip.pick(hit: Bool, -rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -later: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all), hs: AllIn(+Sigma<U32, _ => Rsp.Outcome>, later, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip.pick(hit, rest, later), all):
  match hit:
    case True{}:
      hr
    case False{}:
      hs

def committed_write_succeeds.skip(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.skip(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, +hr):
          committed_write_succeeds.skip.pick(U32.is_eq(r, req), rest, Pol.skip(rest, req), all, hr, committed_write_succeeds.skip(rest, req, all, hr))

def committed_write_succeeds.after.pick(hit: Bool, rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hr: AllIn(+Sigma<U32, _ => Rsp.Outcome>, rest, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after.pick(hit, rest, req), all):
  match hit:
    case True{}:
      hr
    case False{}:
      committed_write_succeeds.skip(rest, req, all, hr)

def committed_write_succeeds.after(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> AllIn(+Sigma<U32, _ => Rsp.Outcome>, Pol.after(left, req), all):
  match left:
    case Nil{}:
      Unit{}
    case (r, o) <> +rest:
      match hl:
        case (m, hr):
          committed_write_succeeds.after.pick(U32.is_eq(r, req), rest, req, all, hr)

# The entry's own Send: every field matches and the reply is a success.
def committed_write_succeeds.hit(+a: Bool, +b: Bool, +c: Bool, +d: Bool, -x: Bool, ha: {a == True{} : Bool}, hb: {b == True{} : Bool}, hc: {c == True{} : Bool}, hd: {d == True{} : Bool}) -> {Bool.or(Bool.and(Bool.and(a, b), Bool.and(c, d)), x) == True{} : Bool}:
  %Equal.sym(Bool, a, True{}, ha) : {Bool.or(Bool.and(Bool.and(_, b), Bool.and(c, d)), x) == True{} : Bool}
  %Equal.sym(Bool, b, True{}, hb) : {Bool.or(Bool.and(Bool.and(True{}, _), Bool.and(c, d)), x) == True{} : Bool}
  %Equal.sym(Bool, c, True{}, hc) : {Bool.or(Bool.and(Bool.and(True{}, True{}), Bool.and(_, d)), x) == True{} : Bool}
  %Equal.sym(Bool, d, True{}, hd) : {Bool.or(Bool.and(Bool.and(True{}, True{}), Bool.and(True{}, _)), x) == True{} : Bool}
  {==}

def committed_write_succeeds.entry(c: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +conn: U32, +gen: U32, +op: Req.Op, +u: U32, -rest: List<&2, Pol.Command>, hc: {Laws.only_commit(all, req, op) == c : Bool}, hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.implies(c, Laws.success_sent(req, conn, gen, Pol.Send{req, conn, gen, Rsp.respond(op, Pol.outcome(True{}, all, left, req), u)} <> rest)) == True{} : Bool}:
  match c:
    case True{}:
      committed_write_succeeds.hit(U32.is_eq(req, req), U32.is_eq(conn, conn), U32.is_eq(gen, gen), Rsp.is_success(Rsp.respond(op, Pol.head(left, all, req), u)), Laws.success_sent(req, conn, gen, rest), u32_eq_refl(req), u32_eq_refl(conn), u32_eq_refl(gen), committed_write_succeeds.success(op, Pol.head(left, all, req), u, committed_write_succeeds.head(left, all, req, op, hl, hc)))
    case False{}:
      {==}

# A command put in front keeps every successful Send.
def committed_write_succeeds.ss_cons(x: Pol.Command, +req: U32, +conn: U32, +gen: U32, -ys: List<&2, Pol.Command>, h: {Laws.success_sent(req, conn, gen, ys) == True{} : Bool}) -> {Laws.success_sent(req, conn, gen, x <> ys) == True{} : Bool}:
  match x:
    case Pol.Send{r, cn, g, reply}:
      %Equal.sym(Bool, Laws.success_sent(req, conn, gen, ys), True{}, h) : {Bool.or(Bool.and(Bool.and(U32.is_eq(r, req), U32.is_eq(cn, conn)), Bool.and(U32.is_eq(g, gen), Rsp.is_success(reply))), _) == True{} : Bool}
      or_true_r(Bool.and(Bool.and(U32.is_eq(r, req), U32.is_eq(cn, conn)), Bool.and(U32.is_eq(g, gen), Rsp.is_success(reply))))
    case Pol.Queue{b, r, op}:
      h
    case Pol.Open{b, k}:
      h

def committed_write_succeeds.ca_imp(a: Bool, x: Pol.Command, +req: U32, +conn: U32, +gen: U32, -ys: List<&2, Pol.Command>, h: {Laws.implies(a, Laws.success_sent(req, conn, gen, ys)) == True{} : Bool}) -> {Laws.implies(a, Laws.success_sent(req, conn, gen, x <> ys)) == True{} : Bool}:
  match a:
    case True{}:
      committed_write_succeeds.ss_cons(x, req, conn, gen, ys, h)
    case False{}:
      {==}

def committed_write_succeeds.ca_cons(+outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, es: List<&2, Pol.Pending>, +x: Pol.Command, +ys: List<&2, Pol.Command>, +h: {Laws.commits_answered(outs, es, ys) == True{} : Bool}) -> {Laws.commits_answered(outs, es, x <> ys) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +tail:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          and_intro(Laws.implies(Laws.only_commit(outs, req, op), Laws.success_sent(req, conn, gen, x <> ys)), Laws.commits_answered(outs, tail, x <> ys), committed_write_succeeds.ca_imp(Laws.only_commit(outs, req, op), x, req, conn, gen, ys, and_l(Laws.implies(Laws.only_commit(outs, req, op), Laws.success_sent(req, conn, gen, ys)), Laws.commits_answered(outs, tail, ys), h)), committed_write_succeeds.ca_cons(outs, tail, x, ys, and_r(Laws.implies(Laws.only_commit(outs, req, op), Laws.success_sent(req, conn, gen, ys)), Laws.commits_answered(outs, tail, ys), h)))

def committed_write_succeeds.bt_imp(a: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, entries: List<&2, Pol.Pending>, +x: Pol.Command, +ys: List<&2, Pol.Command>, h: {Laws.implies(a, Laws.commits_answered(outs, entries, ys)) == True{} : Bool}) -> {Laws.implies(a, Laws.commits_answered(outs, entries, x <> ys)) == True{} : Bool}:
  match a:
    case True{}:
      committed_write_succeeds.ca_cons(outs, entries, x, ys, h)
    case False{}:
      {==}

def committed_write_succeeds.bt_cons(bt: Pol.Batch, +b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +x: Pol.Command, +ys: List<&2, Pol.Command>, h: {Laws.batch_answered(b, outs, bt, ys) == True{} : Bool}) -> {Laws.batch_answered(b, outs, bt, x <> ys) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      committed_write_succeeds.bt_imp(U32.is_eq(id, b), outs, entries, x, ys, h)

def committed_write_succeeds.ba_cons(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, bs: List<&2, Pol.Batch>, +x: Pol.Command, +ys: List<&2, Pol.Command>, +h: {Laws.batches_answered(b, outs, bs, ys) == True{} : Bool}) -> {Laws.batches_answered(b, outs, bs, x <> ys) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      and_intro(Laws.batch_answered(b, outs, bt, x <> ys), Laws.batches_answered(b, outs, rest, x <> ys), committed_write_succeeds.bt_cons(bt, b, outs, x, ys, and_l(Laws.batch_answered(b, outs, bt, ys), Laws.batches_answered(b, outs, rest, ys), h)), committed_write_succeeds.ba_cons(b, outs, rest, x, ys, and_r(Laws.batch_answered(b, outs, bt, ys), Laws.batches_answered(b, outs, rest, ys), h)))

def committed_write_succeeds.ba_answer(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +bs: List<&2, Pol.Batch>, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +rest: List<&2, Pol.Command>, h: {Laws.batches_answered(b, outs, bs, rest) == True{} : Bool}) -> {Laws.batches_answered(b, outs, bs, Pol.answer(True{}, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      h
    case p <> +tail:
      match p:
        case Pol.Pending{+req, conn, gen, op}:
          committed_write_succeeds.ba_cons(b, outs, bs, Pol.Send{req, conn, gen, Rsp.respond(op, Pol.outcome(True{}, all, left, req), u)}, Pol.answer(True{}, all, u, tail, Pol.after(left, req), rest), committed_write_succeeds.ba_answer(b, outs, bs, all, u, tail, Pol.after(left, req), rest, h))

# The replies of one batch.
def committed_write_succeeds.answer(+all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, es: List<&2, Pol.Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +rest: List<&2, Pol.Command>, +hl: AllIn(+Sigma<U32, _ => Rsp.Outcome>, left, all)) -> {Laws.commits_answered(all, es, Pol.answer(True{}, all, u, es, left, rest)) == True{} : Bool}:
  match es:
    case Nil{}:
      {==}
    case p <> +tail:
      match p:
        case Pol.Pending{+req, +conn, +gen, +op}:
          and_intro(Laws.implies(Laws.only_commit(all, req, op), Laws.success_sent(req, conn, gen, Pol.Send{req, conn, gen, Rsp.respond(op, Pol.outcome(True{}, all, left, req), u)} <> Pol.answer(True{}, all, u, tail, Pol.after(left, req), rest))), Laws.commits_answered(all, tail, Pol.Send{req, conn, gen, Rsp.respond(op, Pol.outcome(True{}, all, left, req), u)} <> Pol.answer(True{}, all, u, tail, Pol.after(left, req), rest)), committed_write_succeeds.entry(Laws.only_commit(all, req, op), all, left, req, conn, gen, op, u, Pol.answer(True{}, all, u, tail, Pol.after(left, req), rest), {==}, hl), committed_write_succeeds.ca_cons(all, tail, Pol.Send{req, conn, gen, Rsp.respond(op, Pol.outcome(True{}, all, left, req), u)}, Pol.answer(True{}, all, u, tail, Pol.after(left, req), rest), committed_write_succeeds.answer(all, u, tail, Pol.after(left, req), rest, committed_write_succeeds.after(left, req, all, hl))))

# The replies of every batch with id b.
def committed_write_succeeds.answer_in.put(hit: Bool, bt: Pol.Batch, +b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +bs: List<&2, Pol.Batch>, +r: List<&2, Pol.Command>, heq: {U32.is_eq(Pol.id_of(bt), b) == hit : Bool}, ih: {Laws.batches_answered(b, outs, bs, r) == True{} : Bool}) -> {Bool.and(Laws.batch_answered(b, outs, bt, Pol.answer_in.put(hit, bt, True{}, outs, u, r)), Laws.batches_answered(b, outs, bs, Pol.answer_in.put(hit, bt, True{}, outs, u, r))) == True{} : Bool}:
  match hit:
    case True{}:
      match bt:
        case Pol.Batch{id, kind, +entries}:
          %Equal.sym(Bool, U32.is_eq(id, b), True{}, heq) : {Bool.and(Laws.implies(_, Laws.commits_answered(outs, entries, Pol.answer(True{}, outs, u, entries, outs, r))), Laws.batches_answered(b, outs, bs, Pol.answer(True{}, outs, u, entries, outs, r))) == True{} : Bool}
          and_intro(Laws.commits_answered(outs, entries, Pol.answer(True{}, outs, u, entries, outs, r)), Laws.batches_answered(b, outs, bs, Pol.answer(True{}, outs, u, entries, outs, r)), committed_write_succeeds.answer(outs, u, entries, outs, r, all_in_self(+Sigma<U32, _ => Rsp.Outcome>, outs)), committed_write_succeeds.ba_answer(b, outs, bs, outs, u, entries, outs, r, ih))
    case False{}:
      match bt:
        case Pol.Batch{id, kind, entries}:
          %Equal.sym(Bool, U32.is_eq(id, b), False{}, heq) : {Bool.and(Laws.implies(_, Laws.commits_answered(outs, entries, r)), Laws.batches_answered(b, outs, bs, r)) == True{} : Bool}
          ih

def committed_write_succeeds.answer_in(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, bs: List<&2, Pol.Batch>) -> {Laws.batches_answered(b, outs, bs, Pol.answer_in(b, True{}, outs, u, bs)) == True{} : Bool}:
  match bs:
    case Nil{}:
      {==}
    case +bt <> +rest:
      committed_write_succeeds.answer_in.put(U32.is_eq(Pol.id_of(bt), b), bt, b, outs, u, rest, Pol.answer_in(b, True{}, outs, u, rest), {==}, committed_write_succeeds.answer_in(b, outs, u, rest))

# settle answers the batches of park(hw, writes, park(hr, reads,
# inflight)) with id b; the law names reads, writes and inflight in that
# order. A batch left out has another id (skip_bt); swap and ins reorder.
def committed_write_succeeds.skip_bt(bt: Pol.Batch, +b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, -cs: List<&2, Pol.Command>, heq: {U32.is_eq(Pol.id_of(bt), b) == False{} : Bool}) -> {Laws.batch_answered(b, outs, bt, cs) == True{} : Bool}:
  match bt:
    case Pol.Batch{id, kind, entries}:
      %Equal.sym(Bool, U32.is_eq(id, b), False{}, heq) : {Laws.implies(_, Laws.commits_answered(outs, entries, cs)) == True{} : Bool}
      {==}

def committed_write_succeeds.swap(+a: Bool, -x: Bool, -c: Bool, h: {Bool.and(a, Bool.and(x, c)) == True{} : Bool}) -> {Bool.and(x, Bool.and(a, c)) == True{} : Bool}:
  match a:
    case True{}:
      h
    case False{}:
      false_ne_true({Bool.and(x, Bool.and(False{}, c)) == True{} : Bool}, h)

def committed_write_succeeds.ins(-a: Bool, -w: Bool, -c: Bool, hw: {w == True{} : Bool}, h: {Bool.and(a, c) == True{} : Bool}) -> {Bool.and(a, Bool.and(w, c)) == True{} : Bool}:
  %Equal.sym(Bool, w, True{}, hw) : {Bool.and(a, Bool.and(_, c)) == True{} : Bool}
  h

def committed_write_succeeds.settle(hr: Bool, hw: Bool, +b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>, er: {U32.is_eq(Pol.id_of(reads), b) == hr : Bool}, ew: {U32.is_eq(Pol.id_of(writes), b) == hw : Bool}) -> {Laws.batches_answered(b, outs, reads <> writes <> inflight, Pol.opened(hr, next, Pol.Read{}, Pol.opened(hw, Pol.bump(hr, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(hw, writes, Pol.park(hr, reads, inflight)))))) == True{} : Bool}:
  match hr:
    case True{}:
      match hw:
        case True{}:
          committed_write_succeeds.swap(Laws.batch_answered(b, outs, writes, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.batch_answered(b, outs, reads, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.batches_answered(b, outs, inflight, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))))), committed_write_succeeds.ba_cons(b, outs, writes <> reads <> inflight, Pol.Open{next, Pol.Read{}}, Pol.opened(True{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight)))), committed_write_succeeds.ba_cons(b, outs, writes <> reads <> inflight, Pol.Open{Pol.bump(True{}, next), Pol.Write{}}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(True{}, reads, inflight))), committed_write_succeeds.answer_in(b, outs, u, writes <> reads <> inflight))))
        case False{}:
          committed_write_succeeds.ins(Laws.batch_answered(b, outs, reads, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.batch_answered(b, outs, writes, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))))), Laws.batches_answered(b, outs, inflight, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight)))))), committed_write_succeeds.skip_bt(writes, b, outs, Pol.opened(True{}, next, Pol.Read{}, Pol.opened(False{}, Pol.bump(True{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))))), ew), committed_write_succeeds.ba_cons(b, outs, reads <> inflight, Pol.Open{next, Pol.Read{}}, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(True{}, reads, inflight))), committed_write_succeeds.answer_in(b, outs, u, reads <> inflight)))
    case False{}:
      match hw:
        case True{}:
          and_intro(Laws.batch_answered(b, outs, reads, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))))), Laws.batches_answered(b, outs, writes <> inflight, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight)))))), committed_write_succeeds.skip_bt(reads, b, outs, Pol.opened(False{}, next, Pol.Read{}, Pol.opened(True{}, Pol.bump(False{}, next), Pol.Write{}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))))), er), committed_write_succeeds.ba_cons(b, outs, writes <> inflight, Pol.Open{Pol.bump(False{}, next), Pol.Write{}}, Pol.answer_in(b, True{}, outs, u, Pol.park(True{}, writes, Pol.park(False{}, reads, inflight))), committed_write_succeeds.answer_in(b, outs, u, writes <> inflight)))
        case False{}:
          and_intro(Laws.batch_answered(b, outs, reads, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), Laws.batches_answered(b, outs, writes <> inflight, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), committed_write_succeeds.skip_bt(reads, b, outs, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), er), and_intro(Laws.batch_answered(b, outs, writes, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), Laws.batches_answered(b, outs, inflight, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight)))), committed_write_succeeds.skip_bt(writes, b, outs, Pol.answer_in(b, True{}, outs, u, Pol.park(False{}, writes, Pol.park(False{}, reads, inflight))), ew), committed_write_succeeds.answer_in(b, outs, u, inflight)))

def committed_write_succeeds.settle_at(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Pol.Batch, +writes: Pol.Batch, +inflight: List<&2, Pol.Batch>) -> {Laws.commits_succeed(b, outs, Pol.State{next, reads, writes, inflight}, Pol.commands(Pol.step(Pol.State{next, reads, writes, inflight}, Pol.Synced{b, True{}, outs, u}))) == True{} : Bool}:
  committed_write_succeeds.settle(U32.is_eq(Pol.id_of(reads), b), U32.is_eq(Pol.id_of(writes), b), b, outs, u, next, reads, writes, inflight, {==}, {==})

def Laws.committed_write_succeeds(s, b, outs, u):
  match s:
    case Pol.State{+next, +reads, +writes, +inflight}:
      committed_write_succeeds.settle_at(b, outs, u, next, reads, writes, inflight)


# The pure round (S24)
# --------------------
# The host runs Pol.round once per Epoll.turn on the round's events, from
# the state the last round left (host/main.bend). The laws speak about
# Pol.run from state0 over a whole trace. Round.is_run says the two folds
# give the same state and the same commands. Round.one and Round.append say
# that a round of one event is that event's step, and that a round over
# es ++ fs is the round over fs from the state es left, its commands after
# es's. Round.rounds is the host's turns over a list of rounds, each round
# from the state the one before left, its commands after theirs, and
# Round.rounds_is_run says it answers Pol.run over all their events in
# order. So every law about Pol.run is a law about what the host was told
# to do over all its turns. None of these is a law in LAWS.bend: they are
# shared defs, used by no law's proof.
#
# Round.go_app is by induction on es, with the step's result t generalised
# (round.go opens t only after it matched es). Round.run_go is by induction
# on es with t and the accumulator generalised. Both rewrite with list
# append's associativity and right unit. Round.rounds_from is by induction
# on the rounds with the result so far generalised, and rewrites with
# Round.go_then and Round.go_app.

def Round.app_nil(+xs: List<&2, Pol.Command>) -> {List.append(&2, Pol.Command, xs, Nil{}) == xs : List<&2, Pol.Command>}:
  match xs:
    case Nil{}:
      {==}
    case +h <> +t:
      %Equal.sym(List<&2, Pol.Command>, List.append(&2, Pol.Command, t, Nil{}), t, Round.app_nil(t)) : {h <> _ == h <> t : List<&2, Pol.Command>}
      {==}

def Round.app_assoc(+xs: List<&2, Pol.Command>, +ys: List<&2, Pol.Command>, +zs: List<&2, Pol.Command>) -> {List.append(&2, Pol.Command, xs, List.append(&2, Pol.Command, ys, zs)) == List.append(&2, Pol.Command, List.append(&2, Pol.Command, xs, ys), zs) : List<&2, Pol.Command>}:
  match xs:
    case Nil{}:
      {==}
    case +h <> +t:
      %Equal.sym(List<&2, Pol.Command>, List.append(&2, Pol.Command, t, List.append(&2, Pol.Command, ys, zs)), List.append(&2, Pol.Command, List.append(&2, Pol.Command, t, ys), zs), Round.app_assoc(t, ys, zs)) : {h <> _ == h <> List.append(&2, Pol.Command, List.append(&2, Pol.Command, t, ys), zs) : List<&2, Pol.Command>}
      {==}

def Round.join_nil(r: Pol.State & List<&2, Pol.Command>) -> {Pol.round.join(Nil{}, r) == r : Pol.State & List<&2, Pol.Command>}:
  match r:
    case (+s, +d):
      {==}

def Round.join_join(+c: List<&2, Pol.Command>, +d: List<&2, Pol.Command>, r: Pol.State & List<&2, Pol.Command>) -> {Pol.round.join(c, Pol.round.join(d, r)) == Pol.round.join(List.append(&2, Pol.Command, c, d), r) : Pol.State & List<&2, Pol.Command>}:
  match r:
    case (+s, +x):
      %Round.app_assoc(c, d, x) : {(s, List.append(&2, Pol.Command, c, List.append(&2, Pol.Command, d, x))) == (s, _) : Pol.State & List<&2, Pol.Command>}
      {==}

def Round.join_go(+c: List<&2, Pol.Command>, +fs: List<&2, Pol.HostEvent>, r: Pol.State & List<&2, Pol.Command>) -> {Pol.round.join(c, Pol.round.go(fs, r)) == Pol.round.go(fs, Pol.round.join(c, r)) : Pol.State & List<&2, Pol.Command>}:
  match fs:
    case Nil{}:
      {==}
    case +f <> +rest:
      match r:
        case (+s, +d):
          Round.join_join(c, d, Pol.round.go(rest, Pol.step(s, f)))

def Round.go_app(+es: List<&2, Pol.HostEvent>, +fs: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>) -> {Pol.round.go(List.append(&2, Pol.HostEvent, es, fs), t) == Pol.round.go(fs, Pol.round.go(es, t)) : Pol.State & List<&2, Pol.Command>}:
  match es:
    case Nil{}:
      {==}
    case +e <> +rest:
      match t:
        case (+s, +c):
          %Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.round.go(List.append(&2, Pol.HostEvent, rest, fs), Pol.step(s, e)), Pol.round.go(fs, Pol.round.go(rest, Pol.step(s, e))), Round.go_app(rest, fs, Pol.step(s, e))) : {Pol.round.join(c, _) == Pol.round.go(fs, Pol.round.join(c, Pol.round.go(rest, Pol.step(s, e)))) : Pol.State & List<&2, Pol.Command>}
          Round.join_go(c, fs, Pol.round.go(rest, Pol.step(s, e)))

# The round over fs after the result r of the events before them: fs's
# steps from r's state, their commands after r's.
def Round.then(r: Pol.State & List<&2, Pol.Command>, +fs: List<&2, Pol.HostEvent>) -> Pol.State & List<&2, Pol.Command>:
  match r:
    case (+s, +c):
      Pol.round.join(c, Pol.round(s, fs))

def Round.go_then(r: Pol.State & List<&2, Pol.Command>, +fs: List<&2, Pol.HostEvent>) -> {Pol.round.go(fs, r) == Round.then(r, fs) : Pol.State & List<&2, Pol.Command>}:
  match r:
    case (+s, +c):
      %Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.round.join(c, Pol.round.go(fs, (s, Nil{}))), Pol.round.go(fs, (s, List.append(&2, Pol.Command, c, Nil{}))), Round.join_go(c, fs, (s, Nil{}))) : {Pol.round.go(fs, (s, c)) == _ : Pol.State & List<&2, Pol.Command>}
      %Equal.sym(List<&2, Pol.Command>, List.append(&2, Pol.Command, c, Nil{}), c, Round.app_nil(c)) : {Pol.round.go(fs, (s, c)) == Pol.round.go(fs, (s, _)) : Pol.State & List<&2, Pol.Command>}
      {==}

def Round.run_go(+es: List<&2, Pol.HostEvent>, t: Pol.State & List<&2, Pol.Command>, +acc: List<&2, Pol.Command>) -> {Pol.run.go(es, t, acc) == Pol.round.join(acc, Pol.round.go(es, t)) : Pol.State & List<&2, Pol.Command>}:
  match es:
    case Nil{}:
      match t:
        case (+s, +c):
          {==}
    case +e <> +rest:
      match t:
        case (+s, +c):
          %Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.run.go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c)), Pol.round.join(List.append(&2, Pol.Command, acc, c), Pol.round.go(rest, Pol.step(s, e))), Round.run_go(rest, Pol.step(s, e), List.append(&2, Pol.Command, acc, c))) : {_ == Pol.round.join(acc, Pol.round.join(c, Pol.round.go(rest, Pol.step(s, e)))) : Pol.State & List<&2, Pol.Command>}
          Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.round.join(acc, Pol.round.join(c, Pol.round.go(rest, Pol.step(s, e)))), Pol.round.join(List.append(&2, Pol.Command, acc, c), Pol.round.go(rest, Pol.step(s, e))), Round.join_join(acc, c, Pol.round.go(rest, Pol.step(s, e))))

# Pol.run and Pol.round give the same state and the same commands.
def Round.is_run(+s: Pol.State, +es: List<&2, Pol.HostEvent>) -> {Pol.run(s, es) == Pol.round(s, es) : Pol.State & List<&2, Pol.Command>}:
  %Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.run.go(es, (s, Nil{}), Nil{}), Pol.round.join(Nil{}, Pol.round.go(es, (s, Nil{}))), Round.run_go(es, (s, Nil{}), Nil{})) : {_ == Pol.round(s, es) : Pol.State & List<&2, Pol.Command>}
  Round.join_nil(Pol.round.go(es, (s, Nil{})))

# A round of one event is that event's step.
def Round.one(+s: Pol.State, +e: Pol.HostEvent) -> {Pol.round(s, [e]) == Pol.step(s, e) : Pol.State & List<&2, Pol.Command>}:
  Round.join_nil(Pol.step(s, e))

# A round over es ++ fs is the round over fs from the state es left, with
# fs's commands after es's.
def Round.append(+s: Pol.State, +es: List<&2, Pol.HostEvent>, +fs: List<&2, Pol.HostEvent>) -> {Pol.round(s, List.append(&2, Pol.HostEvent, es, fs)) == Round.then(Pol.round(s, es), fs) : Pol.State & List<&2, Pol.Command>}:
  %Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.round.go(List.append(&2, Pol.HostEvent, es, fs), (s, Nil{})), Pol.round.go(fs, Pol.round.go(es, (s, Nil{}))), Round.go_app(es, fs, (s, Nil{}))) : {_ == Round.then(Pol.round(s, es), fs) : Pol.State & List<&2, Pol.Command>}
  Round.go_then(Pol.round.go(es, (s, Nil{})), fs)

# The host's turns over a list of rounds: each round folded from the state
# the round before left, its commands after the commands before
# (host/main.bend). Round.rounds.go carries the result so far whole, as
# round.go carries the last step's.
def Round.rounds.go(rs: List<&2, List<&2, Pol.HostEvent>>, r: Pol.State & List<&2, Pol.Command>) -> Pol.State & List<&2, Pol.Command>:
  match rs:
    case Nil{}:
      r
    case +es <> +rest:
      Round.rounds.go(rest, Round.then(r, es))

def Round.rounds(s: Pol.State, rs: List<&2, List<&2, Pol.HostEvent>>) -> Pol.State & List<&2, Pol.Command>:
  Round.rounds.go(rs, (s, Nil{}))

def Round.rounds_from(+rs: List<&2, List<&2, Pol.HostEvent>>, r: Pol.State & List<&2, Pol.Command>) -> {Round.rounds.go(rs, r) == Pol.round.go(List.concat(&2, Pol.HostEvent, rs), r) : Pol.State & List<&2, Pol.Command>}:
  match rs:
    case Nil{}:
      {==}
    case +es <> +rest:
      match r:
        case (+s, +c):
          %Equal.sym(Pol.State & List<&2, Pol.Command>, Round.rounds.go(rest, Round.then((s, c), es)), Pol.round.go(List.concat(&2, Pol.HostEvent, rest), Round.then((s, c), es)), Round.rounds_from(rest, Round.then((s, c), es))) : {_ == Pol.round.go(List.append(&2, Pol.HostEvent, es, List.concat(&2, Pol.HostEvent, rest)), (s, c)) : Pol.State & List<&2, Pol.Command>}
          %Round.go_then((s, c), es) : {Pol.round.go(List.concat(&2, Pol.HostEvent, rest), _) == Pol.round.go(List.append(&2, Pol.HostEvent, es, List.concat(&2, Pol.HostEvent, rest)), (s, c)) : Pol.State & List<&2, Pol.Command>}
          Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.round.go(List.append(&2, Pol.HostEvent, es, List.concat(&2, Pol.HostEvent, rest)), (s, c)), Pol.round.go(List.concat(&2, Pol.HostEvent, rest), Pol.round.go(es, (s, c))), Round.go_app(es, List.concat(&2, Pol.HostEvent, rest), (s, c)))

# The host's rounds, one after another from s, answer what Pol.run answers
# over all their events in order: the same state and the same commands.
def Round.rounds_is_run(+s: Pol.State, +rs: List<&2, List<&2, Pol.HostEvent>>) -> {Round.rounds(s, rs) == Pol.run(s, List.concat(&2, Pol.HostEvent, rs)) : Pol.State & List<&2, Pol.Command>}:
  %Equal.sym(Pol.State & List<&2, Pol.Command>, Round.rounds.go(rs, (s, Nil{})), Pol.round.go(List.concat(&2, Pol.HostEvent, rs), (s, Nil{})), Round.rounds_from(rs, (s, Nil{}))) : {_ == Pol.run(s, List.concat(&2, Pol.HostEvent, rs)) : Pol.State & List<&2, Pol.Command>}
  Equal.sym(Pol.State & List<&2, Pol.Command>, Pol.run(s, List.concat(&2, Pol.HostEvent, rs)), Pol.round(s, List.concat(&2, Pol.HostEvent, rs)), Round.is_run(s, List.concat(&2, Pol.HostEvent, rs)))

# Two events from state0: a GET /feed (request 1 on connection 7) is queued
# in the read batch 0, then the host flushes batch 0 and read batch 2 opens.
# The round answers the Queue, then the Open, in step order. The round over
# the second event after the round over the first answers the same, and so
# do the two events as two rounds folded by Round.rounds.

def Witness.round_arrived() -> Pol.HostEvent:
  Pol.Arrived{1, 7, 1, Req.Request{Req.Feed{}, Req.NoBearer{}, Req.NoneSent{}}}

def Witness.round_result() -> Pol.State & List<&2, Pol.Command>:
  (Pol.State{3, Pol.Batch{2, Pol.Read{}, Nil{}}, Pol.Batch{1, Pol.Write{}, Nil{}}, [Pol.Batch{0, Pol.Read{}, [Pol.Pending{1, 7, 1, Req.FeedQuery{}}]}]}, [Pol.Queue{0, 1, Req.FeedQuery{}}, Pol.Open{2, Pol.Read{}}])

def Witness.round_two_events() -> {Pol.round(Pol.state0(), [Witness.round_arrived(), Pol.Flushed{0}]) == Witness.round_result() : Pol.State & List<&2, Pol.Command>}:
  {==}

def Witness.round_two_events.split() -> {Round.then(Pol.round(Pol.state0(), [Witness.round_arrived()]), [Pol.Flushed{0}]) == Witness.round_result() : Pol.State & List<&2, Pol.Command>}:
  {==}

def Witness.round_two_events.rounds() -> {Round.rounds(Pol.state0(), [[Witness.round_arrived()], [Pol.Flushed{0}]]) == Witness.round_result() : Pol.State & List<&2, Pol.Command>}:
  {==}


# Non-vacuity
# -----------
# The laws applied at concrete inputs. Witness.trace has a create and a
# like in the write batch and a health check in the read batch, both
# batches flushed (write first) and synced (read first). Witness.commands
# computes its commands, three success replies among them, so
# success_after_commit, one_reply_per_request and
# reply_to_the_same_generation are not vacuous on it, and it shows the read
# batch answered before the write batch flushed before it. The hypothesis
# of one_reply_per_request holds there for request 1, and that of
# reject_commands_nothing for a request with a bad token. Witness.pending
# is a state with one create in flight, so the Synced of
# unknown_is_not_success answers something.

def Witness.ts() -> String:
  "2026-11-08T12:00:00Z"

def Witness.create(req: U32, conn: U32, text: String) -> Pol.HostEvent:
  Pol.Arrived{req, conn, 1, Req.Request{Req.CreatePost{}, Req.User{3, "ann"}, Req.Trimmed{text}}}

def Witness.trace() -> List<&2, Pol.HostEvent>:
  [Witness.create(1, 10, "hello"), Pol.Arrived{2, 11, 1, Req.Request{Req.LikePost{Req.ValidId{7}}, Req.User{3, "ann"}, Req.NoneSent{}}}, Pol.Arrived{4, 13, 2, Req.Request{Req.Health{}, Req.NoBearer{}, Req.NoneSent{}}}, Pol.Flushed{1}, Pol.Flushed{0}, Pol.Synced{0, True{}, [(4, Rsp.Ping{})], 42}, Pol.Synced{1, True{}, [(1, Rsp.Committed{41, Witness.ts()}), (2, Rsp.Liked{False{}})], 5}]

def Witness.commands() -> {Pol.commands(Pol.run(Pol.state0(), Witness.trace())) == [Pol.Queue{1, 1, Req.InsertPost{3, "ann", "hello"}}, Pol.Queue{1, 2, Req.InsertLike{3, 7}}, Pol.Queue{0, 4, Req.SelectOne{}}, Pol.Open{2, Pol.Write{}}, Pol.Open{3, Pol.Read{}}, Pol.Send{4, 13, 2, Rsp.Health{42}}, Pol.Send{2, 11, 1, Rsp.LikedReply{201, False{}, 7}}, Pol.Send{1, 10, 1, Rsp.Created{41, "hello", Witness.ts(), "ann", 0}}] : List<&2, Pol.Command>}:
  {==}

def Witness.success_after_commit() -> {Laws.success_backed(Witness.trace(), Pol.commands(Pol.run(Pol.state0(), Witness.trace())), Pol.commands(Pol.run(Pol.state0(), Witness.trace()))) == True{} : Bool}:
  Laws.success_after_commit(Witness.trace())

def Witness.one_reply_per_request() -> {Laws.at_most_once(Laws.replies(1, Pol.commands(Pol.run(Pol.state0(), Witness.trace())))) == True{} : Bool}:
  Laws.one_reply_per_request(Witness.trace(), 1, {==})

def Witness.reply_to_the_same_generation() -> {Laws.sends_match_arrivals(Witness.trace(), Pol.commands(Pol.run(Pol.state0(), Witness.trace()))) == True{} : Bool}:
  Laws.reply_to_the_same_generation(Witness.trace())

def Witness.pending() -> Pol.State:
  Pol.State{4, Pol.Batch{0, Pol.Read{}, Nil{}}, Pol.Batch{2, Pol.Write{}, Nil{}}, [Pol.Batch{1, Pol.Write{}, [Pol.Pending{1, 10, 1, Req.InsertPost{3, "ann", "hello"}}]}]}

def Witness.unknown_is_not_success() -> {Laws.missing_fails(Nil{}, Pol.commands(Pol.step(Witness.pending(), Pol.Synced{1, True{}, Nil{}, 5}))) == True{} : Bool}:
  Laws.unknown_is_not_success(Witness.pending(), 1, True{}, Nil{}, 5)

def Witness.unknown_is_not_success.replies() -> {Pol.commands(Pol.step(Witness.pending(), Pol.Synced{1, True{}, Nil{}, 5})) == [Pol.Send{1, 10, 1, Rsp.Error{500, "internal server error"}}] : List<&2, Pol.Command>}:
  {==}

def Witness.reject_commands_nothing() -> {Laws.send_count(Pol.commands(Pol.step(Pol.state0(), Pol.Arrived{3, 12, 1, Req.Request{Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}}}))) == Laws.Once{} : Laws.Count} & {Laws.queue_count(Pol.commands(Pol.step(Pol.state0(), Pol.Arrived{3, 12, 1, Req.Request{Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}}}))) == Laws.Never{} : Laws.Count}:
  Laws.reject_commands_nothing(Pol.state0(), 3, 12, 1, Req.Request{Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}}, {==})

# failed_batch_no_success on the trace where the database connection of
# write batch 1 dies before batch 1 is flushed: request 1 is queued in
# batch 1, Died{1} opens write batch 2 and answers request 1 with 500,
# request 2 then joins batch 2, and the Synced{2, True{}, ..} answers
# request 2 with 201. The law holds for request 1, and the 201 to request 2
# shows the run after the failure does carry a success.

def Witness.fb_pre() -> List<&2, Pol.HostEvent>:
  [Witness.create(1, 10, "hello")]

def Witness.fb_post() -> List<&2, Pol.HostEvent>:
  [Witness.create(2, 11, "again"), Pol.Flushed{2}, Pol.Synced{2, True{}, [(2, Rsp.Committed{42, Witness.ts()})], 5}]

def Witness.failed_batch_no_success() -> {Laws.no_success_to(1, Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Synced{1, False{}, Nil{}, 5} <> Witness.fb_post()))) == True{} : Bool} & {Laws.no_success_to(1, Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Died{1} <> Witness.fb_post()))) == True{} : Bool}:
  Laws.failed_batch_no_success(Witness.fb_pre(), Witness.fb_post(), 1, 1, Nil{}, 5, {==}, {==})

def Witness.failed_batch_no_success.replies() -> {Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Died{1} <> Witness.fb_post())) == [Pol.Open{2, Pol.Write{}}, Pol.Send{1, 10, 1, Rsp.Error{500, "internal server error"}}, Pol.Queue{2, 2, Req.InsertPost{3, "ann", "again"}}, Pol.Open{3, Pol.Write{}}, Pol.Send{2, 11, 1, Rsp.Created{42, "again", Witness.ts(), "ann", 0}}] : List<&2, Pol.Command>}:
  {==}

# gone_no_reply on the trace where request 1's connection (10, 1) closes
# while its create is queued: after the Gone the commit answers nobody,
# and without the Gone the same events answer request 1 on (10, 1).
# Witness.gone_no_reply.unconditional is the counterexample to the law
# without its hypothesis: an Arrived on (10, 1) after Gone{10, 1} whose
# request classify rejects is answered at once on (10, 1).

def Witness.gn_post() -> List<&2, Pol.HostEvent>:
  [Pol.Flushed{1}, Pol.Synced{1, True{}, [(1, Rsp.Committed{41, Witness.ts()})], 5}]

def Witness.gone_no_reply() -> {Laws.no_send_to(10, 1, Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Gone{10, 1} <> Witness.gn_post()))) == True{} : Bool}:
  Laws.gone_no_reply(Witness.fb_pre(), Witness.gn_post(), 10, 1, {==})

def Witness.gone_no_reply.replies() -> {Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Gone{10, 1} <> Witness.gn_post())) == [Pol.Open{2, Pol.Write{}}] : List<&2, Pol.Command>}:
  {==}

def Witness.gone_no_reply.without() -> {Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Witness.gn_post())) == [Pol.Open{2, Pol.Write{}}, Pol.Send{1, 10, 1, Rsp.Created{41, "hello", Witness.ts(), "ann", 0}}] : List<&2, Pol.Command>}:
  {==}

def Witness.gone_no_reply.unconditional() -> {Laws.no_send_to(10, 1, Pol.commands(Pol.run(Laws.state_after(Witness.fb_pre()), Pol.Gone{10, 1} <> [Pol.Arrived{5, 10, 1, Req.Request{Req.CreatePost{}, Req.BadToken{}, Req.Trimmed{"hi"}}}]))) == False{} : Bool}:
  {==}

# committed_write_succeeds on a state whose write batch 1 is in flight with
# a like (request 2, newest) and a create (request 1), and a Synced whose
# outs are in statement order, not newest first: the like misses the head
# of outs and is answered from the fallback, the create from the head of
# what is left. Both pairs are the only ones of their request and commit
# its op, and both replies are successes.

def Witness.cw_state() -> Pol.State:
  Pol.State{4, Pol.Batch{0, Pol.Read{}, Nil{}}, Pol.Batch{2, Pol.Write{}, Nil{}}, [Pol.Batch{1, Pol.Write{}, [Pol.Pending{2, 11, 1, Req.InsertLike{3, 7}}, Pol.Pending{1, 10, 1, Req.InsertPost{3, "ann", "hello"}}]}]}

def Witness.cw_outs() -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  [(1, Rsp.Committed{41, Witness.ts()}), (2, Rsp.Liked{False{}})]

def Witness.committed_write_succeeds() -> {Laws.commits_succeed(1, Witness.cw_outs(), Witness.cw_state(), Pol.commands(Pol.step(Witness.cw_state(), Pol.Synced{1, True{}, Witness.cw_outs(), 5}))) == True{} : Bool}:
  Laws.committed_write_succeeds(Witness.cw_state(), 1, Witness.cw_outs(), 5)

def Witness.committed_write_succeeds.premise() -> {Bool.and(Laws.only_commit(Witness.cw_outs(), 2, Req.InsertLike{3, 7}), Laws.only_commit(Witness.cw_outs(), 1, Req.InsertPost{3, "ann", "hello"})) == True{} : Bool}:
  {==}

def Witness.committed_write_succeeds.replies() -> {Pol.commands(Pol.step(Witness.cw_state(), Pol.Synced{1, True{}, Witness.cw_outs(), 5})) == [Pol.Send{2, 11, 1, Rsp.LikedReply{201, False{}, 7}}, Pol.Send{1, 10, 1, Rsp.Created{41, "hello", Witness.ts(), "ann", 0}}] : List<&2, Pol.Command>}:
  {==}

# post_exact, like_exact and post_query_exact at one request each: user 3
# ("ann") posts "hi", likes post 7 with no body, and reads post 7 with no
# token and no body.

def Witness.post_exact() -> {Req.classify(Req.Request{Req.CreatePost{}, Req.User{3, "ann"}, Req.Trimmed{"hi"}}) == Req.Run{Req.InsertPost{3, "ann", "hi"}} : Req.Decision}:
  Laws.post_exact(3, "ann", "hi")

def Witness.like_exact() -> {Req.classify(Req.Request{Req.LikePost{Req.ValidId{7}}, Req.User{3, "ann"}, Req.NoneSent{}}) == Req.Run{Req.InsertLike{3, 7}} : Req.Decision}:
  Laws.like_exact(3, "ann", 7, Req.NoneSent{})

def Witness.post_query_exact() -> {Req.classify(Req.Request{Req.PostById{Req.ValidId{7}}, Req.NoBearer{}, Req.NoneSent{}}) == Req.Run{Req.PostQuery{7}} : Req.Decision}:
  Laws.post_query_exact(7, Req.NoBearer{}, Req.NoneSent{})
