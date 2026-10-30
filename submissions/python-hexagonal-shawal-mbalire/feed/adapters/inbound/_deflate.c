/*
 * _deflate: gzip for response bodies, through libdeflate (built from its pinned source by build.sh).
 *
 * zlib's stream API allocates and initialises its state (window, hash chains) for every stream,
 * and Python's zlib module opens a new stream per call. libdeflate compresses a whole buffer in one
 * call with a compressor allocated once, so a response costs the compression itself and nothing
 * else: no malloc, no memset of hash tables, no heap growth handed back to the kernel.
 *
 * One compressor per process. Workers are forked after nothing has called gzip() yet, and the
 * event loop is single-threaded, so the compressor is never shared.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "libdeflate.h"

#define LEVEL 1  /* the fastest level; on the feed it still beats zlib level 1 on size */

static struct libdeflate_compressor *compressor;

static PyObject *gzip(PyObject *self, PyObject *arg) {
    Py_buffer in;
    if (PyObject_GetBuffer(arg, &in, PyBUF_SIMPLE) < 0) return NULL;
    size_t cap = libdeflate_gzip_compress_bound(compressor, in.len);
    PyObject *out = PyBytes_FromStringAndSize(NULL, cap);
    if (!out) { PyBuffer_Release(&in); return NULL; }
    size_t n = libdeflate_gzip_compress(compressor, in.buf, in.len, PyBytes_AS_STRING(out), cap);
    PyBuffer_Release(&in);
    if (!n) {  /* cannot happen with a bound-sized buffer; fail loudly if it ever does */
        Py_DECREF(out);
        PyErr_SetString(PyExc_RuntimeError, "libdeflate: output did not fit its own bound");
        return NULL;
    }
    if (_PyBytes_Resize(&out, (Py_ssize_t)n) < 0) return NULL;
    return out;
}

static PyMethodDef methods[] = {
    {"gzip", gzip, METH_O, "bytes -> the same bytes in a gzip container (deflate level 1)."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_deflate", NULL, -1, methods};

PyMODINIT_FUNC PyInit__deflate(void) {
    if (!compressor && !(compressor = libdeflate_alloc_compressor(LEVEL))) return PyErr_NoMemory();
    return PyModule_Create(&module);
}
