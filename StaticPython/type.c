
// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/type.h"

#include "cinderx/StaticPython/vtable.h"

#include "structmember.h"

Py_ssize_t
_PyClassLoader_PrimitiveTypeToSize(int primitive_type)
{
    switch (primitive_type) {
    case TYPED_INT8:
        return sizeof(char);
    case TYPED_INT16:
        return sizeof(short);
    case TYPED_INT32:
        return sizeof(int);
    case TYPED_INT64:
        return sizeof(long);
    case TYPED_UINT8:
        return sizeof(unsigned char);
    case TYPED_UINT16:
        return sizeof(unsigned short);
    case TYPED_UINT32:
        return sizeof(unsigned int);
    case TYPED_UINT64:
        return sizeof(unsigned long);
    case TYPED_BOOL:
        return sizeof(char);
    case TYPED_DOUBLE:
        return sizeof(double);
    case TYPED_SINGLE:
        return sizeof(float);
    case TYPED_CHAR:
        return sizeof(char);
    case TYPED_OBJECT:
        return sizeof(PyObject *);
    default:
        PyErr_Format(
            PyExc_ValueError, "unknown struct type: %d", primitive_type);
        return -1;
    }
}

int
_PyClassLoader_PrimitiveTypeToStructMemberType(int primitive_type)
{
    switch (primitive_type) {
    case TYPED_INT8:
        return T_BYTE;
    case TYPED_INT16:
        return T_SHORT;
    case TYPED_INT32:
        return T_INT;
    case TYPED_INT64:
        return T_LONG;
    case TYPED_UINT8:
        return T_UBYTE;
    case TYPED_UINT16:
        return T_USHORT;
    case TYPED_UINT32:
        return T_UINT;
    case TYPED_UINT64:
        return T_ULONG;
    case TYPED_BOOL:
        return T_BOOL;
    case TYPED_DOUBLE:
        return T_DOUBLE;
    case TYPED_SINGLE:
        return T_FLOAT;
    case TYPED_CHAR:
        return T_CHAR;
    case TYPED_OBJECT:
        return T_OBJECT_EX;
    default:
        PyErr_Format(
            PyExc_ValueError, "unknown struct type: %d", primitive_type);
        return -1;
    }
}

PyObject *
_PyClassLoader_Box(uint64_t value, int primitive_type)
{
    PyObject* new_val;
    double dbl;
    switch (primitive_type) {
        case TYPED_BOOL:
            new_val = value ? Py_True : Py_False;
            Py_INCREF(new_val);
            break;
        case TYPED_INT8:
            new_val = PyLong_FromLong((int8_t)value);
            break;
        case TYPED_INT16:
            new_val = PyLong_FromLong((int16_t)value);
            break;
        case TYPED_INT32:
            new_val = PyLong_FromLong((int32_t)value);
            break;
        case TYPED_INT64:
            new_val = PyLong_FromSsize_t((Py_ssize_t)value);
            break;
        case TYPED_UINT8:
            new_val = PyLong_FromUnsignedLong((uint8_t)value);
            break;
        case TYPED_UINT16:
            new_val = PyLong_FromUnsignedLong((uint16_t)value);
            break;
        case TYPED_UINT32:
            new_val = PyLong_FromUnsignedLong((uint32_t)value);
            break;
        case TYPED_UINT64:
            new_val = PyLong_FromSize_t((size_t)value);
            break;
        case TYPED_DOUBLE:
            memcpy(&dbl, &value, sizeof(double));
            new_val = PyFloat_FromDouble(dbl);
            break;
        default:
            assert(0);
            PyErr_SetString(PyExc_RuntimeError, "unsupported primitive type");
            new_val = NULL;
            break;
    }
    return new_val;
}

uint64_t
_PyClassLoader_Unbox(PyObject *value, int primitive_type)
{
    uint64_t new_val;
    double res;
    switch (primitive_type) {
        case TYPED_BOOL:
            new_val = value == Py_True ? 1 : 0;
            break;
        case TYPED_INT8:
        case TYPED_INT16:
        case TYPED_INT32:
        case TYPED_INT64:
            new_val = (uint64_t)PyLong_AsLong(value);
            break;
        case TYPED_UINT8:
        case TYPED_UINT16:
        case TYPED_UINT32:
        case TYPED_UINT64:
            new_val = (uint64_t)PyLong_AsUnsignedLong(value);
            break;
        case TYPED_DOUBLE:
            res = PyFloat_AsDouble(value);
            memcpy(&new_val, &res, sizeof(double));
            break;
        default:
            assert(0);
            PyErr_SetString(PyExc_RuntimeError, "unsupported primitive type");
            new_val = 0;
            break;
    }
    return new_val;
}

int _PyClassLoader_GetTypeCode(PyTypeObject *type) {
    if (type->tp_cache == NULL) {
        return TYPED_OBJECT;
    }

    return ((_PyType_VTable *)type->tp_cache)->vt_typecode;
}
