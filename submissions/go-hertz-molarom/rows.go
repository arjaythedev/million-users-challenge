package main

import (
	"errors"
	"fmt"

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
// https://github.com/jackc/pgx/blob/b2ef08bbb951384e3936e45de60de60609f2d2ba/rows.go#L257
type bufferedRows struct {
	fields []pgconn.FieldDescription
	values [][]byte
	ncols  int
	nrows  int
	tag    pgconn.CommandTag
	err    error

	cur     int
	closed  bool
	typeMap *pgtype.Map
}

var _ pgx.Rows = (*bufferedRows)(nil)

func bufferRows(src pgx.Rows) *bufferedRows {
	defer src.Close()

	sfd := src.FieldDescriptions()
	r := &bufferedRows{
		fields: make([]pgconn.FieldDescription, len(sfd)),
		cur:    -1,
	}
	r.ncols = len(r.fields)
	copy(r.fields, sfd)

	arena := make([]byte, 0, 512)
	var bounds []int // start, end per value; start -1 = NULL
	for src.Next() {
		for _, v := range src.RawValues() {
			if v == nil {
				bounds = append(bounds, -1, -1)
				continue
			}
			bounds = append(bounds, len(arena), len(arena)+len(v))
			arena = append(arena, v...)
		}
	}
	src.Close()

	r.err = src.Err()
	r.tag = src.CommandTag()
	r.typeMap = src.TypeMap().Copy()

	r.values = make([][]byte, len(bounds)/2)
	for i := range r.values {
		if start := bounds[2*i]; start >= 0 {
			r.values[i] = arena[start:bounds[2*i+1]:bounds[2*i+1]]
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
		return r.fatal(fmt.Errorf("number of field descriptions must equal number of destinations, got %d and %d", r.ncols, len(dest)))
	}

	if len(dest) == 1 {
		if rs, ok := dest[0].(pgx.RowScanner); ok {
			return r.fatal(rs.ScanRow(r))
		}
	}

	raw := r.RawValues()
	if raw == nil {
		return r.fatal(errors.New("scan called without a current row"))
	}

	for i, d := range dest {
		if d == nil {
			continue
		}
		fd := &r.fields[i]
		if err := r.typeMap.Scan(fd.DataTypeOID, fd.Format, raw[i], d); err != nil {
			return r.fatal(pgx.ScanArgError{ColumnIndex: i, FieldName: fd.Name, Err: err})
		}
	}
	return nil
}

// Values decodes the current row.
func (r *bufferedRows) Values() ([]any, error) {
	raw := r.RawValues()
	if raw == nil {
		return nil, errors.New("values called without a current row")
	}

	values := make([]any, len(raw))
	for i, buf := range raw {
		if buf == nil {
			values = append(values, nil)
			continue
		}

		fd := &r.fields[i]
		if dt, ok := r.typeMap.TypeForOID(fd.DataTypeOID); ok {
			v, err := dt.Codec.DecodeValue(r.typeMap, fd.DataTypeOID, fd.Format, buf)
			if err != nil {
				return nil, r.fatal(err)
			}
			values[i] = v
			continue
		}

		switch fd.Format {
		case pgtype.TextFormatCode:
			values[i] = string(buf)
		case pgtype.BinaryFormatCode:
			newBuf := make([]byte, len(buf))
			copy(newBuf, buf)
			values[i] = newBuf
		default:
			return nil, r.fatal(errors.New("unknown format code"))
		}

		if r.Err() != nil {
			return nil, r.Err()
		}
	}
	return values, r.Err()
}

func (r *bufferedRows) fatal(err error) error {
	if err == nil {
		return nil
	}
	if r.err == nil {
		r.err = err
	}
	r.Close()
	return err
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
