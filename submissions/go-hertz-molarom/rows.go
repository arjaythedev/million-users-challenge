package main

import (
	"errors"
	"fmt"
	"slices"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"
)

// bufferedRows is a concurrency safe implementation of pgx.Rows.
//
// pgx.Rows are bound to the lifetime of the underlying connection.
// We cannot guarantee a connection's lifetime in the worker pool implementation,
// so we have to buffer the row(s) into memory for later usage.
//
// adapted from:
// https://github.com/jackc/pgx/blob/b2ef08bbb951384e3936e45de60de60609f2d2ba/rows.go
type bufferedRows struct {
	fields []pgconn.FieldDescription // shared per statement; read-only
	values [][]byte
	ncols  int
	nrows  int
	tag    pgconn.CommandTag
	err    error

	cur     int
	closed  bool
	typeMap *pgtype.Map

	// backs values for single-row results (getPost, INSERT ... RETURNING).
	inline [inlineValues][]byte
}

const inlineValues = 5

var _ pgx.Rows = (*bufferedRows)(nil)

// rowScratch is reusable space for collecting a result's raw values.
type rowScratch struct {
	arena  []byte
	bounds []int32 // start, end per value; start -1 = NULL

	// field descriptions (with result formats) per prepared statement.
	fields map[*pgconn.StatementDescription][]pgconn.FieldDescription
}

func newRowScratch() rowScratch {
	return rowScratch{
		arena:  make([]byte, 0, 4096),
		bounds: make([]int32, 0, 256),
		fields: make(map[*pgconn.StatementDescription][]pgconn.FieldDescription),
	}
}

func (s *rowScratch) fieldsFor(sd *pgconn.StatementDescription, src []pgconn.FieldDescription) []pgconn.FieldDescription {
	if f, ok := s.fields[sd]; ok && len(f) == len(src) {
		return f
	}
	f := slices.Clone(src)
	if sd != nil {
		s.fields[sd] = f
	}
	return f
}

func bufferRows(src pgx.Rows, sd *pgconn.StatementDescription, s *rowScratch) *bufferedRows {
	defer src.Close()

	r := &bufferedRows{
		fields: s.fieldsFor(sd, src.FieldDescriptions()),
		cur:    -1,
	}
	r.ncols = len(r.fields)

	arena, bounds := s.arena[:0], s.bounds[:0]
	for src.Next() {
		for _, v := range src.RawValues() {
			if v == nil {
				bounds = append(bounds, -1, -1)
				continue
			}
			bounds = append(bounds, int32(len(arena)), int32(len(arena)+len(v)))
			arena = append(arena, v...)
		}
	}
	src.Close()
	s.arena, s.bounds = arena, bounds // keep any growth for the next result

	r.err = src.Err()
	r.tag = src.CommandTag()
	r.typeMap = src.TypeMap().Copy()

	slab := make([]byte, len(arena))
	copy(slab, arena)

	if n := len(bounds) / 2; n <= len(r.inline) {
		r.values = r.inline[:n]
	} else {
		r.values = make([][]byte, n)
	}
	for i := range r.values {
		if start := bounds[2*i]; start >= 0 {
			end := bounds[2*i+1]
			r.values[i] = slab[start:end:end]
		}
	}
	if r.ncols > 0 {
		r.nrows = len(r.values) / r.ncols
	}
	return r
}

// errRows is a [pgx.Rows] that returns an error.
func errRows(err error) *bufferedRows {
	return &bufferedRows{err: err, cur: -1, closed: true}
}

func (r *bufferedRows) Err() error                                   { return r.err }
func (r *bufferedRows) CommandTag() pgconn.CommandTag                { return r.tag }
func (r *bufferedRows) FieldDescriptions() []pgconn.FieldDescription { return r.fields }
func (r *bufferedRows) TypeMap() *pgtype.Map                         { return r.typeMap }
func (r *bufferedRows) Conn() *pgx.Conn                              { return nil }

func (r *bufferedRows) Close() {
	if r.closed {
		return
	}
	r.closed = true
}

// Next advances to the next row.
func (r *bufferedRows) Next() bool {
	if r.closed {
		return false
	}
	if r.cur+1 >= r.nrows {
		r.Close()
		return false
	}
	r.cur++
	return true
}

func (r *bufferedRows) RawValues() [][]byte {
	if r.cur < 0 || r.cur >= r.nrows {
		return nil
	}
	return r.values[r.cur*r.ncols : (r.cur+1)*r.ncols]
}

func (r *bufferedRows) Scan(dest ...any) error {
	if len(dest) != r.ncols {
		r.fatal(fmt.Errorf("number of field descriptions must equal number of destinations, got %d and %d", r.ncols, len(dest)))
		return r.Err()
	}

	if len(dest) == 1 {
		if rs, ok := dest[0].(pgx.RowScanner); ok {
			err := rs.ScanRow(r)
			if err != nil {
				r.fatal(err)
			}
			return err
		}
	}

	raw := r.RawValues()
	if raw == nil {
		err := errors.New("scan called without a current row")
		r.fatal(err)
		return err
	}

	for i, d := range dest {
		if d == nil {
			continue
		}
		fd := &r.fields[i]
		if err := r.typeMap.Scan(fd.DataTypeOID, fd.Format, raw[i], d); err != nil {
			r.fatal(pgx.ScanArgError{ColumnIndex: i, FieldName: fd.Name, Err: err})
			return r.Err()
		}
	}
	return nil
}

// Values decodes the current row.
func (r *bufferedRows) Values() ([]any, error) {
	if r.closed {
		return nil, errors.New("rows is closed")
	}

	raw := r.RawValues()
	if raw == nil {
		return nil, errors.New("values called without a current row")
	}

	values := make([]any, 0, len(r.FieldDescriptions()))
	for i := range r.FieldDescriptions() {
		buf := raw[i]
		fd := &r.FieldDescriptions()[i]

		if buf == nil {
			values = append(values, nil)
			continue
		}

		if dt, ok := r.typeMap.TypeForOID(fd.DataTypeOID); ok {
			v, err := dt.Codec.DecodeValue(r.typeMap, fd.DataTypeOID, fd.Format, buf)
			if err != nil {
				r.fatal(err)
			}
			values = append(values, v)
		} else {
			switch fd.Format {
			case pgtype.TextFormatCode:
				values = append(values, string(buf))
			case pgtype.BinaryFormatCode:
				newBuf := make([]byte, len(buf))
				copy(newBuf, buf)
				values = append(values, newBuf)
			default:
				r.fatal(errors.New("unknown format code"))
			}
		}

		if r.Err() != nil {
			return nil, r.Err()
		}
	}
	return values, r.Err()
}

func (r *bufferedRows) fatal(err error) {
	if r.err != nil {
		return
	}
	r.err = err
	r.Close()
}

// row implements pgx.Row.
type row struct {
	rows pgx.Rows
	err  error
}

var _ pgx.Row = (*row)(nil)

func (r *row) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}

	rows := r.rows
	defer rows.Close()

	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return err
		}
		return pgx.ErrNoRows
	}
	if err := rows.Scan(dest...); err != nil {
		return err
	}
	rows.Close()
	return rows.Err()
}
