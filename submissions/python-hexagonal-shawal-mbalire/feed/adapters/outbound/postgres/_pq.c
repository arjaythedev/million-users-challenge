/*
 * _pq: a minimal CPython binding to libpq in pipeline mode, for one event loop.
 *
 * Why it exists: asyncpg runs one query per connection at a time, so each in-flight query costs a
 * connection. libpq's pipeline mode (PG 14+) lets one connection carry many queries in flight,
 * answered in order, and lets a single write(2) carry every query queued during one event-loop
 * tick. The Python side (pg_libpq.py) owns scheduling; this file owns only what must be fast:
 *
 *   send(name, params)  PQsendQueryPrepared + a sync point. Text parameters; nothing is flushed.
 *   flush()             PQflush: 0 = all sent, 1 = socket full, wait for writability.
 *   read()              PQconsumeInput, then every *complete* reply as a Python object, in order.
 *
 * One sync point per query keeps failures independent: a statement that fails (a like on a missing
 * post) aborts the pipeline only up to the next sync, i.e. only itself.
 *
 * Rows are decoded straight into the tuples the domain uses (PostView), by column type:
 * int2/int4/int8 -> int, timestamptz -> the wire string "YYYY-MM-DDTHH:MM:SS.mmmZ" (truncated, see
 * feed/domain/timefmt.py, which this mirrors), everything else -> str. The session must run with
 * TimeZone=UTC and DateStyle=ISO, which pg_libpq.py sets before entering pipeline mode.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

#define INT2OID 21
#define INT4OID 23
#define INT8OID 20
#define TIMESTAMPOID 1114
#define TIMESTAMPTZOID 1184
#define MAX_PARAMS 8

static PyObject *PqError; /* PqError(sqlstate, message) */

typedef struct {
    PyObject_HEAD
    PGconn *conn;
    Py_ssize_t queued;  /* sync points sent whose PGRES_PIPELINE_SYNC has not been read yet */
    PyObject *current;  /* the decoded reply of the query in progress, until its sync arrives */
} Conn;

static PyObject *error_from_conn(PGconn *conn) {
    PyObject *e = PyObject_CallFunction(PqError, "ss", "08006", conn ? PQerrorMessage(conn) : "no connection");
    if (e) { PyErr_SetObject(PqError, e); Py_DECREF(e); }
    return NULL;
}

static PyObject *iso_millis(const char *v, int len) {
    /* "2025-12-31 23:59:19.409123+00" -> "2025-12-31T23:59:19.409Z" */
    if (len < 19) return PyUnicode_DecodeUTF8(v, len, NULL);
    char out[24];
    memcpy(out, v, 19);
    out[10] = 'T';
    out[19] = '.';
    int i = 0;
    if (len > 19 && v[19] == '.')
        for (; i < 3 && 20 + i < len && v[20 + i] >= '0' && v[20 + i] <= '9'; i++) out[20 + i] = v[20 + i];
    for (; i < 3; i++) out[20 + i] = '0';
    out[23] = 'Z';
    return PyUnicode_FromStringAndSize(out, 24);
}

static PyObject *decode_value(const PGresult *r, int row, int col, Oid type) {
    if (PQgetisnull(r, row, col)) Py_RETURN_NONE;
    const char *v = PQgetvalue(r, row, col);
    int len = PQgetlength(r, row, col);
    switch (type) {
    case INT2OID: case INT4OID: case INT8OID:
        return PyLong_FromLongLong(strtoll(v, NULL, 10));
    case TIMESTAMPTZOID: case TIMESTAMPOID:
        return iso_millis(v, len);
    default:
        return PyUnicode_DecodeUTF8(v, len, NULL);
    }
}

/* A finished query -> list[tuple] (rows), int (rows affected), or a PqError instance (not raised). */
static PyObject *decode_result(const PGresult *r) {
    switch (PQresultStatus(r)) {
    case PGRES_TUPLES_OK: {
        int rows = PQntuples(r), cols = PQnfields(r);
        Oid types[16];
        if (cols > 16) return PyObject_CallFunction(PqError, "ss", "XX000", "too many columns");
        for (int c = 0; c < cols; c++) types[c] = PQftype(r, c);
        PyObject *list = PyList_New(rows);
        if (!list) return NULL;
        for (int i = 0; i < rows; i++) {
            PyObject *t = PyTuple_New(cols);
            if (!t) { Py_DECREF(list); return NULL; }
            PyList_SET_ITEM(list, i, t);
            for (int c = 0; c < cols; c++) {
                PyObject *v = decode_value(r, i, c, types[c]);
                if (!v) { Py_DECREF(list); return NULL; }
                PyTuple_SET_ITEM(t, c, v);
            }
        }
        return list;
    }
    case PGRES_COMMAND_OK:
        return PyLong_FromLong(atol(PQcmdTuples((PGresult *)r)));
    default: {
        const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        const char *msg = PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY);
        return PyObject_CallFunction(PqError, "ss", state ? state : "XX000",
                                     msg ? msg : PQresStatus(PQresultStatus(r)));
    }
    }
}

static int Conn_init(Conn *self, PyObject *args, PyObject *kwds) {
    const char *conninfo;
    if (!PyArg_ParseTuple(args, "s", &conninfo)) return -1;
    PGconn *conn;
    Py_BEGIN_ALLOW_THREADS
    conn = PQconnectdb(conninfo);
    Py_END_ALLOW_THREADS
    if (PQstatus(conn) != CONNECTION_OK) {
        error_from_conn(conn);
        PQfinish(conn);
        return -1;
    }
    self->conn = conn;
    return 0;
}

static void Conn_dealloc(Conn *self) {
    if (self->conn) PQfinish(self->conn);
    Py_XDECREF(self->current);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

#define REQUIRE_CONN(self) if (!(self)->conn) return error_from_conn(NULL)

/* Blocking: for session setup and PREPARE, before entering pipeline mode. */
static PyObject *Conn_execute(Conn *self, PyObject *args) {
    const char *sql;
    if (!PyArg_ParseTuple(args, "s", &sql)) return NULL;
    REQUIRE_CONN(self);
    PGresult *r;
    Py_BEGIN_ALLOW_THREADS
    r = PQexec(self->conn, sql);
    Py_END_ALLOW_THREADS
    ExecStatusType st = PQresultStatus(r);
    PQclear(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) return error_from_conn(self->conn);
    Py_RETURN_NONE;
}

static PyObject *Conn_prepare(Conn *self, PyObject *args) {
    const char *name, *sql;
    if (!PyArg_ParseTuple(args, "ss", &name, &sql)) return NULL;
    REQUIRE_CONN(self);
    PGresult *r;
    Py_BEGIN_ALLOW_THREADS
    r = PQprepare(self->conn, name, sql, 0, NULL);
    Py_END_ALLOW_THREADS
    ExecStatusType st = PQresultStatus(r);
    PQclear(r);
    if (st != PGRES_COMMAND_OK) return error_from_conn(self->conn);
    Py_RETURN_NONE;
}

static PyObject *Conn_enter_pipeline(Conn *self, PyObject *unused) {
    REQUIRE_CONN(self);
    if (PQsetnonblocking(self->conn, 1) != 0 || PQenterPipelineMode(self->conn) != 1)
        return error_from_conn(self->conn);
    Py_RETURN_NONE;
}

static PyObject *Conn_send(Conn *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 2 || !PyUnicode_Check(args[0]) || !PyTuple_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError, "send(name: str, params: tuple)");
        return NULL;
    }
    REQUIRE_CONN(self);
    const char *name = PyUnicode_AsUTF8(args[0]);
    if (!name) return NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args[1]);
    if (n > MAX_PARAMS) { PyErr_SetString(PyExc_ValueError, "too many parameters"); return NULL; }
    const char *values[MAX_PARAMS];
    PyObject *keep[MAX_PARAMS] = {0};
    PyObject *ret = NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *v = PyTuple_GET_ITEM(args[1], i);
        if (v == Py_None) { values[i] = NULL; continue; }
        keep[i] = PyUnicode_Check(v) ? Py_NewRef(v) : PyObject_Str(v);
        if (!keep[i] || !(values[i] = PyUnicode_AsUTF8(keep[i]))) goto done;
    }
    int ok = PQsendQueryPrepared(self->conn, name, (int)n, values, NULL, NULL, 0);
#ifdef LIBPQ_HAS_SEND_PIPELINE_SYNC
    if (ok) ok = PQsendPipelineSync(self->conn);  /* PG 17+: a sync point without a flush */
#else
    if (ok) ok = PQpipelineSync(self->conn);      /* older libpq: also attempts a flush */
#endif
    if (!ok) { error_from_conn(self->conn); goto done; }
    self->queued++;
    ret = Py_NewRef(Py_None);
done:
    for (Py_ssize_t i = 0; i < n; i++) Py_XDECREF(keep[i]);
    return ret;
}

static PyObject *Conn_flush(Conn *self, PyObject *unused) {
    REQUIRE_CONN(self);
    int r = PQflush(self->conn);
    if (r < 0) return error_from_conn(self->conn);
    return PyLong_FromLong(r);
}

static PyObject *Conn_read(Conn *self, PyObject *unused) {
    REQUIRE_CONN(self);
    if (!PQconsumeInput(self->conn)) return error_from_conn(self->conn);
    PyObject *out = PyList_New(0);
    if (!out) return NULL;
    int nulls = 0;
    /* PQisBusy first, always: in nonblocking mode PQgetResult on a busy connection would wait. */
    while (self->queued > 0 && !PQisBusy(self->conn)) {
        PGresult *r = PQgetResult(self->conn);
        if (!r) {               /* end of one query's results */
            if (++nulls > 1) break;  /* defensive: never spin if no progress is possible */
            continue;
        }
        nulls = 0;
        if (PQresultStatus(r) == PGRES_PIPELINE_SYNC) {
            PyObject *item = self->current ? self->current : Py_NewRef(Py_None);
            self->current = NULL;
            self->queued--;
            int failed = PyList_Append(out, item);
            Py_DECREF(item);
            if (failed) { PQclear(r); Py_DECREF(out); return NULL; }
        } else if (!self->current) {
            self->current = decode_result(r);
            if (!self->current) { PQclear(r); Py_DECREF(out); return NULL; }
        }
        PQclear(r);
    }
    return out;
}

static PyObject *Conn_fileno(Conn *self, PyObject *unused) {
    REQUIRE_CONN(self);
    return PyLong_FromLong(PQsocket(self->conn));
}

static PyObject *Conn_close(Conn *self, PyObject *unused) {
    if (self->conn) { PQfinish(self->conn); self->conn = NULL; }
    Py_RETURN_NONE;
}

static PyObject *Conn_get_queued(Conn *self, void *closure) { return PyLong_FromSsize_t(self->queued); }

static PyMethodDef Conn_methods[] = {
    {"execute", (PyCFunction)Conn_execute, METH_VARARGS, "Blocking statement (setup only)."},
    {"prepare", (PyCFunction)Conn_prepare, METH_VARARGS, "Blocking PREPARE (setup only)."},
    {"enter_pipeline", (PyCFunction)Conn_enter_pipeline, METH_NOARGS, "Nonblocking + pipeline mode."},
    {"send", (PyCFunction)(void (*)(void))Conn_send, METH_FASTCALL, "Queue a prepared statement and a sync."},
    {"flush", (PyCFunction)Conn_flush, METH_NOARGS, "0 = sent, 1 = wait for writability."},
    {"read", (PyCFunction)Conn_read, METH_NOARGS, "Every complete reply, in order."},
    {"fileno", (PyCFunction)Conn_fileno, METH_NOARGS, NULL},
    {"close", (PyCFunction)Conn_close, METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef Conn_getset[] = {
    {"queued", (getter)Conn_get_queued, NULL, "Replies still outstanding.", NULL},
    {NULL}
};

static PyTypeObject ConnType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_pq.Conn",
    .tp_basicsize = sizeof(Conn),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Conn_init,
    .tp_dealloc = (destructor)Conn_dealloc,
    .tp_methods = Conn_methods,
    .tp_getset = Conn_getset,
};

static PyObject *libpq_version(PyObject *m, PyObject *unused) { return PyLong_FromLong(PQlibVersion()); }

static PyObject *py_iso_millis(PyObject *m, PyObject *arg) {
    Py_ssize_t len;
    const char *v = PyUnicode_AsUTF8AndSize(arg, &len);
    return v ? iso_millis(v, (int)len) : NULL;
}

static PyMethodDef module_methods[] = {
    {"libpq_version", libpq_version, METH_NOARGS, NULL},
    {"iso_millis", py_iso_millis, METH_O, "The timestamptz decoder, exposed for tests."},
    {NULL}
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_pq", NULL, -1, module_methods};

PyMODINIT_FUNC PyInit__pq(void) {
    if (PyType_Ready(&ConnType) < 0) return NULL;
    PyObject *m = PyModule_Create(&module);
    if (!m) return NULL;
    PqError = PyErr_NewException("_pq.PqError", NULL, NULL);
    if (PyModule_AddObjectRef(m, "PqError", PqError) < 0 ||
        PyModule_AddObjectRef(m, "Conn", (PyObject *)&ConnType) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
