/*
 * _cjson: the two hot JSON encoders, post(view) and feed(views), written in C.
 *
 * A PostView is the tuple (id:int, body:str, created_at:str, author:str, like_count:int). The
 * output is byte-identical to the hand-written Python codec (tests/test_codecs.py checks it),
 * including its escaping, which is json.encoder.encode_basestring's: \" \\ \b \f \n \r \t, other
 * control characters as \u00XX, everything else (non-ASCII included) copied as UTF-8.
 *
 * One growable buffer per call, one PyBytes at the end. No intermediate Python objects: strings
 * are read through PyUnicode_AsUTF8AndSize, which for ASCII text returns the object's own storage.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; Py_ssize_t len, cap; char inline_buf[4096]; } Buf;

static int grow(Buf *b, Py_ssize_t need) {
    if (b->len + need <= b->cap) return 0;
    Py_ssize_t cap = b->cap * 2;
    while (cap < b->len + need) cap *= 2;
    char *p = b->p == b->inline_buf ? PyMem_Malloc(cap) : PyMem_Realloc(b->p, cap);
    if (!p) { PyErr_NoMemory(); return -1; }
    if (b->p == b->inline_buf) memcpy(p, b->inline_buf, b->len);
    b->p = p; b->cap = cap;
    return 0;
}

static void buf_init(Buf *b) { b->p = b->inline_buf; b->len = 0; b->cap = sizeof b->inline_buf; }
static void buf_free(Buf *b) { if (b->p != b->inline_buf) PyMem_Free(b->p); }

static int put(Buf *b, const char *s, Py_ssize_t n) {
    if (grow(b, n) < 0) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}
#define PUT_LIT(b, s) put((b), (s), (Py_ssize_t)(sizeof(s) - 1))

static int put_int(Buf *b, PyObject *o) {
    long long v = PyLong_AsLongLong(o);
    if (v == -1 && PyErr_Occurred()) return -1;
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    return put(b, tmp, n);
}

static int put_str(Buf *b, PyObject *o) {
    Py_ssize_t n;
    const char *s = PyUnicode_AsUTF8AndSize(o, &n);
    if (!s) return -1;
    if (grow(b, n * 6 + 2) < 0) return -1;  /* worst case: every byte becomes \u00XX */
    char *w = b->p + b->len;
    *w++ = '"';
    for (Py_ssize_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') { *w++ = (char)c; continue; }
        *w++ = '\\';
        switch (c) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '\b': *w++ = 'b'; break;
        case '\f': *w++ = 'f'; break;
        case '\n': *w++ = 'n'; break;
        case '\r': *w++ = 'r'; break;
        case '\t': *w++ = 't'; break;
        default: {
            static const char hex[] = "0123456789abcdef";
            *w++ = 'u'; *w++ = '0'; *w++ = '0'; *w++ = hex[c >> 4]; *w++ = hex[c & 15];
        }
        }
    }
    *w++ = '"';
    b->len = w - b->p;
    return 0;
}

static int put_post(Buf *b, PyObject *view) {
    if (!PyTuple_Check(view) || PyTuple_GET_SIZE(view) != 5) {
        PyErr_SetString(PyExc_TypeError, "a post view is a 5-tuple");
        return -1;
    }
    return PUT_LIT(b, "{\"id\":") || put_int(b, PyTuple_GET_ITEM(view, 0))
        || PUT_LIT(b, ",\"body\":") || put_str(b, PyTuple_GET_ITEM(view, 1))
        || PUT_LIT(b, ",\"created_at\":") || put_str(b, PyTuple_GET_ITEM(view, 2))
        || PUT_LIT(b, ",\"author\":") || put_str(b, PyTuple_GET_ITEM(view, 3))
        || PUT_LIT(b, ",\"like_count\":") || put_int(b, PyTuple_GET_ITEM(view, 4))
        || PUT_LIT(b, "}") ? -1 : 0;
}

static PyObject *finish(Buf *b, int failed) {
    PyObject *out = failed ? NULL : PyBytes_FromStringAndSize(b->p, b->len);
    buf_free(b);
    return out;
}

static PyObject *post(PyObject *m, PyObject *view) {
    Buf b; buf_init(&b);
    int failed = PUT_LIT(&b, "{\"post\":") || put_post(&b, view) || PUT_LIT(&b, "}");
    return finish(&b, failed);
}

static PyObject *feed(PyObject *m, PyObject *views) {
    PyObject *seq = PySequence_Fast(views, "feed() takes a sequence of post views");
    if (!seq) return NULL;
    Buf b; buf_init(&b);
    int failed = PUT_LIT(&b, "{\"posts\":[");
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);
    for (Py_ssize_t i = 0; i < n && !failed; i++)
        failed = (i && PUT_LIT(&b, ",")) || put_post(&b, items[i]);
    failed = failed || PUT_LIT(&b, "]}");
    Py_DECREF(seq);
    return finish(&b, failed);
}

static PyMethodDef methods[] = {
    {"post", post, METH_O, "{\"post\":<post>} as bytes"},
    {"feed", feed, METH_O, "{\"posts\":[<post>...]} as bytes"},
    {NULL}
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_cjson", NULL, -1, methods};

PyMODINIT_FUNC PyInit__cjson(void) { return PyModule_Create(&module); }
