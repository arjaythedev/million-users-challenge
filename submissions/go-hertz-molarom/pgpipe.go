package main

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"sync"
	"sync/atomic"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ErrClosed = errors.New("pipeline is closed")

type DBTX interface {
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
}

type Pipe struct {
	pool   *pgxpool.Pool
	reads  *lane // sync per query
	writes *lane // group commit (if configured)
}

type PipeOptions struct {
	ReadWorkers, WriteWorkers int
	MaxBatch                  int
	GroupCommit               bool
}

func NewPipe(pool *pgxpool.Pool, o PipeOptions) *Pipe {
	p := &Pipe{
		pool: pool,
		reads: newLane(
			pool,
			false,
			o.ReadWorkers,
			o.MaxBatch,
		),
		writes: newLane(
			pool,
			o.GroupCommit,
			o.WriteWorkers,
			o.MaxBatch,
		),
	}

	p.reads.Go()
	p.writes.Go()

	return p
}

func (p *Pipe) Reads() DBTX {
	return p.reads
}

func (p *Pipe) Writes() DBTX {
	return p.writes
}

func (p *Pipe) Close(ctx context.Context) error {
	p.reads.Close()
	p.writes.Close()
	p.pool.Close()
	return nil
}

var opPool = sync.Pool{
	New: func() any {
		return &Op{
			done: make(chan result, 1),
		}
	},
}

func getOp() *Op {
	return opPool.Get().(*Op)
}

func putOp(op *Op) {
	opPool.Put(op)
}

type Op struct {
	sql  string
	args []any
	sd   *pgconn.StatementDescription
	done chan result
}

func (o *Op) Prepare(ctx context.Context, c *pgx.Conn) error {
	sd, err := c.Prepare(ctx, o.sql, o.sql)
	if err != nil {
		return err
	}
	o.sd = sd
	return nil
}

func (o *Op) SendStmt(pl *pgconn.Pipeline, typeMap *pgtype.Map, eqb pgx.ExtendedQueryBuilder) error {
	if err := eqb.Build(typeMap, o.sd, o.args); err != nil {
		return err
	}

	// eqb.Build reuses the backing array for the next iteration.
	resultFormats := make([]int16, len(eqb.ResultFormats))
	copy(resultFormats, eqb.ResultFormats)

	pl.SendQueryStatement(o.sd, eqb.ParamValues, eqb.ParamFormats, resultFormats)

	return nil
}

func (o *Op) Read(pl *pgconn.Pipeline, typeMap *pgtype.Map) result {
	res, err := pl.GetResults()
	switch rr := res.(type) {
	case *pgconn.ResultReader:
		rows := bufferRows(pgx.RowsFromResultReader(typeMap, rr))
		return result{tag: rows.CommandTag(), rows: rows, err: rows.err}
	default:
		if err == nil {
			err = fmt.Errorf("pgpipe: pipeline out of sync: expected a result, got %T", res)
		}

		return result{rows: errRows(err), err: err}
	}
}

type result struct {
	tag  pgconn.CommandTag
	rows pgx.Rows
	err  error
}

type lane struct {
	ctx     context.Context
	cancel  context.CancelCauseFunc
	pool    *pgxpool.Pool
	workers []*laneWorker
	ops     chan *Op
	wg      sync.WaitGroup
	stopped *atomic.Bool

	// Config options
	batchSize   int
	groupCommit bool
}

func newLane(p *pgxpool.Pool, groupCommit bool, numWorkers, maxBatch int) *lane {
	l := &lane{
		ctx:     context.Background(),
		pool:    p,
		ops:     make(chan *Op, numWorkers*maxBatch),
		stopped: &atomic.Bool{},

		batchSize:   maxBatch,
		groupCommit: groupCommit,
	}

	l.ctx, l.cancel = context.WithCancelCause(l.ctx)

	l.workers = make([]*laneWorker, numWorkers)
	for i := 0; i < numWorkers; i++ {
		l.workers[i] = &laneWorker{
			lane:   l,
			batch:  make([]*Op, 0, maxBatch),
			staged: make([]result, 0, maxBatch),
		}
	}

	return l
}

func (l *lane) Go() {
	for _, w := range l.workers {
		l.wg.Go(w.run)
	}
}

func (l *lane) Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error) {
	res, err := l.submit(ctx, sql, args...)
	return res.tag, err
}

func (l *lane) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	res, err := l.submit(ctx, sql, args...)
	return res.rows, err
}

func (l *lane) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	rows, err := l.Query(ctx, sql, args...)
	return &row{rows: rows, err: err}
}

func (l *lane) Stopped() bool {
	return l.stopped.Load() || l.ctx.Err() != nil
}

func (l *lane) Close() {
	l.wg.Go(func() {
		l.stopped.Store(true)
		l.cancel(ErrClosed)
	})

	l.wg.Wait()
}

func (l *lane) submit(ctx context.Context, sql string, args ...any) (result, error) {
	if l.Stopped() {
		return result{}, ErrClosed
	}

	op := getOp()
	defer putOp(op)

	op.sql = sql
	op.args = args
	op.sd = nil

	select {
	case <-ctx.Done():
		return result{}, ctx.Err()
	case l.ops <- op:
	}

	res := <-op.done

	return res, res.err
}

type laneWorker struct {
	lane    *lane
	conn    *pgxpool.Conn
	inRetry bool
	batch   []*Op
	staged  []result
}

func (w *laneWorker) run() {
	for {
		if w.lane.Stopped() {
			w.tryReleaseConn()
			return
		}

	drain:
		for len(w.batch) < w.lane.batchSize {
			select {
			case op := <-w.lane.ops:
				w.batch = append(w.batch, op)
			default:
				break drain
			}
		}

		if err := w.tryAcquireConn(); err != nil {
			w.failBatch(err)
			continue
		}

		w.flush()
	}
}

func (w *laneWorker) tryAcquireConn() error {
	if w.conn != nil && !w.conn.Conn().IsClosed() {
		return nil
	}

	conn, err := w.lane.pool.Acquire(w.lane.ctx)
	if err != nil {
		return err
	}
	w.conn = conn
	return nil
}

func (w *laneWorker) tryReleaseConn() {
	if w.conn == nil {
		return
	}

	w.conn.Release()
	w.conn = nil
}

func (w *laneWorker) flush() {
	if len(w.batch) == 0 {
		return
	}

	for len(w.batch) > 0 {
		failed, err := w.processBatch(w.batch)
		if err == nil {
			break
		}

		switch {
		case failed < 0:
			w.failBatch(err)
			w.reset()
			return
		case !isDeadlockOrSerializationError(err):
			w.batch[failed].done <- result{rows: errRows(err), err: err}
			w.batch = slices.Delete(w.batch, failed, failed+1)
		}

		w.staged = w.staged[:0]
		w.inRetry = true
	}

	for i, op := range w.batch {
		op.done <- w.staged[i]
	}

	w.reset()
}

func (w *laneWorker) processBatch(batch []*Op) (int, error) {
	c := w.conn.Conn()

	// 1. Prepare statements
	for i, op := range batch {
		if err := op.Prepare(w.lane.ctx, c); err != nil {
			return i, err
		}
	}

	// 2. Start the pipeline
	pipeline := c.PgConn().StartPipeline(w.lane.ctx)
	defer pipeline.Close()

	eqb := pgx.ExtendedQueryBuilder{}
	typeMap := c.TypeMap()

	// 3. Send statements to pipeline
	for i, op := range batch {
		if err := op.SendStmt(pipeline, typeMap, eqb); err != nil {
			return i, err
		}

		// 3.a Save a sync point for individual queries (pipeline mode).
		// or if we've failed a group commit and are in a re-try.
		if !w.isGroupCommit() {
			pipeline.SendPipelineSync()
		}
	}

	// 4. Sync point for all writes (group commit)
	// or Flush all pending queries (pipeline mode).
	if err := w.syncOrFlush(pipeline); err != nil {
		return -1, err
	}

	// 5. Read the results.
	if w.isGroupCommit() {
		return w.readGroup(c, pipeline, typeMap, batch)
	}

	return w.readPipeline(c, pipeline, typeMap, batch)
}

func (w *laneWorker) syncOrFlush(pl *pgconn.Pipeline) error {
	if w.lane.groupCommit && !w.inRetry {
		return pl.Sync()
	}
	return pl.Flush()
}

func (w *laneWorker) readPipeline(c *pgx.Conn, pl *pgconn.Pipeline, typeMap *pgtype.Map, batch []*Op) (int, error) {
	for _, op := range batch {
		w.staged = append(w.staged, op.Read(pl, typeMap))

		if err := getPipelineSync(pl); err != nil {
			_ = c.Close(w.lane.ctx)
			return -1, err // pipeline sync failure, abort.
		}
	}
	return -1, nil
}

func (w *laneWorker) readGroup(c *pgx.Conn, pl *pgconn.Pipeline, typeMap *pgtype.Map, batch []*Op) (int, error) {
	for i, op := range batch {
		res := op.Read(pl, typeMap)
		if res.err != nil {
			if !isPgError(res.err) {
				_ = c.Close(w.lane.ctx)
				return -1, res.err
			}
			return i, res.err
		}
		w.staged = append(w.staged, res)
	}

	if err := getPipelineSync(pl); err != nil {
		_ = c.Close(w.lane.ctx)
		return -1, err // pipeline sync failure, abort.
	}

	return -1, nil
}

func (w *laneWorker) failBatch(err error) {
	for _, op := range w.batch {
		op.done <- result{rows: errRows(err), err: err}
	}
}

func (w *laneWorker) reset() {
	w.batch = w.batch[:0]
	w.staged = w.staged[:0]
	w.inRetry = false
}

func (w *laneWorker) isGroupCommit() bool {
	return w.lane.groupCommit && !w.inRetry
}

func getPipelineSync(pl *pgconn.Pipeline) error {
	s, err := pl.GetResults()
	if err != nil {
		return err
	}
	if _, ok := s.(*pgconn.PipelineSync); !ok {
		return errors.New("pipeline out of sync")
	}
	return nil
}

func isDeadlockOrSerializationError(err error) bool {
	pe, ok := errors.AsType[*pgconn.PgError](err)
	return (ok && pe.Code == "40P01") || // deadlock
		(ok && pe.Code == "40001") // serialization
}

func isPgError(err error) bool {
	_, ok := errors.AsType[*pgconn.PgError](err)
	return ok
}
