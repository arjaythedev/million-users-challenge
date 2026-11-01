/*
 * _igzip: gzip for response bodies through Intel ISA-L's igzip at level 0 (built by build.sh from
 * its pinned source tarball; its x86-64 assembly needs nasm, which install.sh adds).
 *
 * Level 0 is igzip's fastest mode: a single-pass deflate with a small hash and static Huffman
 * tables, in hand-written SIMD. On the ~3.6 KB feed it took 2.7-3.9 µs against libdeflate level
 * 1's 6.8 µs, for 1,007 bytes instead of 738. CPU is what caps this server; 270 extra bytes per feed
 * is ~42 Mbit/s at a million users.
 *
 * Stateless compression keeps no state between calls: the stream struct lives on the stack and
 * level 0 needs no work buffer, so there is nothing to allocate, reset or share across workers.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "igzip_lib.h"

/* Level 0 never expands by more than its stored-block framing; the slack also covers the header. */
#define BOUND(n) ((n) + (n) / 8 + 1024)

static PyObject *gzip(PyObject *self, PyObject *arg) {
    Py_buffer in;
    if (PyObject_GetBuffer(arg, &in, PyBUF_SIMPLE) < 0) return NULL;
    if (in.len > UINT32_MAX / 2) {
        PyBuffer_Release(&in);
        PyErr_SetString(PyExc_OverflowError, "body too large for igzip");
        return NULL;
    }
    Py_ssize_t cap = BOUND(in.len);
    PyObject *out = PyBytes_FromStringAndSize(NULL, cap);
    if (!out) { PyBuffer_Release(&in); return NULL; }
    struct isal_zstream s;
    isal_deflate_stateless_init(&s);
    s.next_in = in.buf;
    s.avail_in = (uint32_t)in.len;
    s.next_out = (uint8_t *)PyBytes_AS_STRING(out);
    s.avail_out = (uint32_t)cap;
    s.gzip_flag = IGZIP_GZIP;
    s.level = 0;
    int status = isal_deflate_stateless(&s);
    PyBuffer_Release(&in);
    if (status != COMP_OK) {
        Py_DECREF(out);
        PyErr_Format(PyExc_RuntimeError, "igzip: isal_deflate_stateless returned %d", status);
        return NULL;
    }
    if (_PyBytes_Resize(&out, cap - (Py_ssize_t)s.avail_out) < 0) return NULL;
    return out;
}

static PyMethodDef methods[] = {
    {"gzip", gzip, METH_O, "bytes -> the same bytes in a gzip container (igzip level 0)."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_igzip", NULL, -1, methods};

PyMODINIT_FUNC PyInit__igzip(void) { return PyModule_Create(&module); }
