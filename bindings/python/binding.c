#include <Python.h>

#include "tree_sitter/parser.h"

const TSLanguage *tree_sitter_dash(void);

static PyObject *language(PyObject *self, PyObject *args) {
  (void)self;
  (void)args;
  return PyCapsule_New(
    (void *)tree_sitter_dash(),
    "tree_sitter.Language",
    NULL
  );
}

static PyMethodDef methods[] = {
  {"language", language, METH_NOARGS, "Return the dash language capsule."},
  {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT,
  .m_name = "binding",
  .m_size = -1,
  .m_methods = methods,
};

PyMODINIT_FUNC PyInit_binding(void) {
  return PyModule_Create(&module);
}
