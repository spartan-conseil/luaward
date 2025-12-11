#include <Python.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "structmember.h"

#define DEFAULT_MAX_MEMORY (5 * 1024 * 1024)

typedef struct {
    size_t total_allocated;
    size_t max_memory;
    unsigned long long instruction_count;
    unsigned long long instruction_limit;
} MemControl;

static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
    MemControl *mc = (MemControl *)ud;
    if (nsize == 0) {
        if (ptr) {
            // Safe subtraction using builtin
            // size_t subtraction wrapping is "overflow" in these builtins for unsigned types?
            // Yes, for unsigned, it detects wrap-around (underflow).
            size_t new_total;
            if (__builtin_sub_overflow(mc->total_allocated, osize, &new_total)) {
                // Underflow detected, meaning we are trying to free more than accounted
                mc->total_allocated = 0;
            } else {
                mc->total_allocated = new_total;
            }
            free(ptr);
        }
        return NULL;
    }
    else {
        // Calculate current baseline
        size_t current_usage = mc->total_allocated;
        size_t temp_usage;
        if (ptr) {
             // We are reallocating, so strictly speaking we free 'osize' then add 'nsize'.
             // But existing Lua realloc implementations often just look at the diff.
             // Here we emulate free osize then alloc nsize logic to match total_allocated semantics.
             if (__builtin_sub_overflow(current_usage, osize, &temp_usage)) {
                 temp_usage = 0; // Should not happen if accounting is consistent
             }
             current_usage = temp_usage;
        }
        
        // Check for overflow when adding nsize
        size_t new_total;
        if (__builtin_add_overflow(current_usage, nsize, &new_total)) {
            return NULL; // Overflow detected
        }
        
        if (new_total > mc->max_memory) {
            return NULL;
        }

        // Proceed with allocation
        void* newptr = realloc(ptr, nsize);
        if (newptr) {
            mc->total_allocated = new_total;
        }
        return newptr;
    }
}

static void instruction_count_hook(lua_State *L, lua_Debug *ar) {
    MemControl *mc;
    lua_getallocf(L, (void **)&mc);
    
    // We set the hook count to 1000, so we increment by 1000
    mc->instruction_count += 1000;
    
    if (mc->instruction_limit > 0 && mc->instruction_count > mc->instruction_limit) {
        luaL_error(L, "Instruction limit exceeded");
    }
}

typedef struct {
    PyObject_HEAD
    lua_State *L;
    MemControl mc;
    PyObject* callbacks; // Dictionary of name -> callable
} LuaVM;

static void LuaVM_dealloc(LuaVM *self) {
    Py_XDECREF(self->callbacks);
    if (self->L) {
        lua_close(self->L);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int convert_python_to_lua(lua_State *L, PyObject *arg) {
    if (arg == Py_None) {
        lua_pushnil(L);
    } else if (PyBool_Check(arg)) {
        lua_pushboolean(L, (arg == Py_True));
    } else if (PyLong_Check(arg)) {
        long long val = PyLong_AsLongLong(arg);
        lua_pushinteger(L, val);
    } else if (PyFloat_Check(arg)) {
        double val = PyFloat_AsDouble(arg);
        lua_pushnumber(L, val);
    } else if (PyUnicode_Check(arg)) {
        const char *s = PyUnicode_AsUTF8(arg);
        lua_pushstring(L, s);
    } else {
        return -1; // Unsupported
    }
    return 0;
}

static PyObject* convert_lua_to_python(lua_State *L, int index) {
    int type = lua_type(L, index);
    switch (type) {
        case LUA_TNIL:
            Py_RETURN_NONE;
        case LUA_TBOOLEAN:
            return PyBool_FromLong(lua_toboolean(L, index));
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                return PyLong_FromLongLong(lua_tointeger(L, index));
            } else {
                return PyFloat_FromDouble(lua_tonumber(L, index));
            }
        case LUA_TSTRING:
            return PyUnicode_FromString(lua_tostring(L, index));
        default:
            Py_RETURN_NONE; // Return None for others
    }
}


// Generic C-side wrapper for Python upvalue callbacks
static int lua_callback_generic(lua_State *L) {
    // Upvalue 1 is the Python callable (wrapped in a capsule or just managed via invalid pointer logic?
    // Actually, we can't push PyObject* directly to Lua as a value we can retrieve unless we use lightuserdata.
    // lightuserdata is just a pointer. As long as the PyObject is alive, it's fine.
    // The PyObject is alive because it's in self->callbacks dict.
    
    void *ptr = lua_touserdata(L, lua_upvalueindex(1));
    if (!ptr) {
        return luaL_error(L, "Internal error: callback pointer missing");
    }
    PyObject *func = (PyObject *)ptr;

    PyGILState_STATE gstate = PyGILState_Ensure();

    // Collect arguments
    int nargs = lua_gettop(L);
    PyObject *py_args = PyTuple_New(nargs);
    for (int i = 0; i < nargs; i++) {
        PyObject *arg_obj = convert_lua_to_python(L, i + 1);
        PyTuple_SetItem(py_args, i, arg_obj); // Steals reference
    }

    PyObject *result = PyObject_CallObject(func, py_args);
    Py_DECREF(py_args);

    if (result == NULL) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return luaL_error(L, "Python callback raised an exception");
    }

    // Convert result back
    // We handle None, string, int, float, bool.
    // If tuple, return multiple values? For now just one return value support.
    
    if (result == Py_None) {
        lua_pushnil(L);
    } else if (PyBool_Check(result)) {
        lua_pushboolean(L, (result == Py_True));
    } else if (PyLong_Check(result)) {
        lua_pushinteger(L, PyLong_AsLongLong(result));
    } else if (PyFloat_Check(result)) {
        lua_pushnumber(L, PyFloat_AsDouble(result));
    } else if (PyUnicode_Check(result)) {
        lua_pushstring(L, PyUnicode_AsUTF8(result));
    } else {
        // Try convert to string as fallback?
        PyObject *s = PyObject_Str(result);
        if (s) {
            lua_pushstring(L, PyUnicode_AsUTF8(s));
            Py_DECREF(s);
        } else {
             lua_pushnil(L);
        }
    }
    
    Py_DECREF(result);
    PyGILState_Release(gstate);
    return 1;
}

static int LuaVM_init(LuaVM *self, PyObject *args, PyObject *kwds) {
    unsigned long long max_mem = DEFAULT_MAX_MEMORY;
    unsigned long long instr_limit = 0;
    PyObject *callbacks_dict = NULL;
    static char *kwlist[] = {"memory_limit", "callbacks", "instruction_limit", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|KOK", kwlist, &max_mem, &callbacks_dict, &instr_limit)) {
        return -1;
    }

    self->mc.total_allocated = 0;
    self->mc.max_memory = (size_t)max_mem;
    self->mc.instruction_limit = instr_limit;
    self->mc.instruction_count = 0;
    
    self->L = lua_newstate(l_alloc, &self->mc);

    if (self->L == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create Lua state (Memory Limit?)");
        return -1;
    }
    
    self->callbacks = NULL;
    if (callbacks_dict && PyDict_Check(callbacks_dict)) {
        self->callbacks = callbacks_dict;
        Py_INCREF(self->callbacks);
    }

    lua_State *L = self->L;

    // Sandbox setup
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    const char* prohibited[] = {
        "dofile", "loadfile", "load", "module", "require", NULL
    };
    for (int i = 0; prohibited[i] != NULL; i++) {
        lua_pushnil(L);
        lua_setglobal(L, prohibited[i]);
    }
    
    // Register callbacks from dict
    if (self->callbacks) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(self->callbacks, &pos, &key, &value)) {
             if (PyUnicode_Check(key) && PyCallable_Check(value)) {
                 const char *func_name = PyUnicode_AsUTF8(key);
                 lua_pushlightuserdata(L, (void*)value); // Push function pointer as upvalue
                 lua_pushcclosure(L, lua_callback_generic, 1);
                 lua_setglobal(L, func_name);
             }
        }
    }

    return 0;
}



static PyObject *LuaVM_call(LuaVM *self, PyObject *args) {
    if (self->L == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Lua VM is closed");
        return NULL;
    }

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError, "call expects at least function name");
        return NULL;
    }

    PyObject *name_obj = PyTuple_GetItem(args, 0);
    if (!PyUnicode_Check(name_obj)) {
        PyErr_SetString(PyExc_TypeError, "function name must be a string");
        return NULL;
    }
    const char *func_name = PyUnicode_AsUTF8(name_obj);

    lua_getglobal(self->L, func_name);
    if (!lua_isfunction(self->L, -1)) {
        lua_pop(self->L, 1);
        PyErr_Format(PyExc_RuntimeError, "Global '%s' is not a function", func_name);
        return NULL;
    }

    int nargs = (int)PyTuple_Size(args) - 1;
    for (int i = 0; i < nargs; i++) {
        PyObject *arg = PyTuple_GetItem(args, i + 1);
        if (convert_python_to_lua(self->L, arg) < 0) {
            PyErr_Format(PyExc_TypeError, "Unsupported argument type at index %d", i);
            // Clean up stack? lua_close/pcall handles it mostly but we should proper clean
            // Actually simpler to just verify args first?
            // For now just error out.
            return NULL;
        }
    }

    // Reset instruction count
    self->mc.instruction_count = 0;
    if (self->mc.instruction_limit > 0) {
        lua_sethook(self->L, instruction_count_hook, LUA_MASKCOUNT, 1000);
    } else {
        lua_sethook(self->L, NULL, 0, 0);
    }

    // Call with nargs arguments and 1 return value (supported for now)
    int status = lua_pcall(self->L, nargs, 1, 0);
    
    // Disable hook after call
    lua_sethook(self->L, NULL, 0, 0);
    
    if (status != LUA_OK) {
        const char *error_msg = lua_tostring(self->L, -1);
        if (strcmp(error_msg, "Instruction limit exceeded") == 0) {
             PyErr_SetString(PyExc_TimeoutError, "Instruction limit exceeded");
        } else {
             PyErr_Format(PyExc_RuntimeError, "Lua error: %s", error_msg);
        }
        lua_pop(self->L, 1);
        return NULL;
    }

    PyObject *ret = convert_lua_to_python(self->L, -1);
    lua_pop(self->L, 1);
    return ret;
}

static PyObject *LuaVM_execute(LuaVM *self, PyObject *args) {
    const char *script;
    if (!PyArg_ParseTuple(args, "s", &script)) {
        return NULL;
    }
    
    if (self->L == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Lua VM is closed");
        return NULL;
    }

    // Reset instruction count
    self->mc.instruction_count = 0;
    if (self->mc.instruction_limit > 0) {
        lua_sethook(self->L, instruction_count_hook, LUA_MASKCOUNT, 1000);
    } else {
        lua_sethook(self->L, NULL, 0, 0);
    }

    int status = luaL_dostring(self->L, script);
    
    // Disable hook after execution
    lua_sethook(self->L, NULL, 0, 0);

    if (status != LUA_OK) {
        const char *error_msg = lua_tostring(self->L, -1);
        if (strcmp(error_msg, "Instruction limit exceeded") == 0) {
             PyErr_SetString(PyExc_TimeoutError, "Instruction limit exceeded");
        } else {
             PyErr_Format(PyExc_RuntimeError, "Lua error: %s", error_msg);
        }
        lua_pop(self->L, 1); // Pop error message
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *LuaVM_function_exists(LuaVM *self, PyObject *args) {
    const char *func_name;
    if (!PyArg_ParseTuple(args, "s", &func_name)) {
        return NULL;
    }

    if (self->L == NULL) {
        Py_RETURN_FALSE;
    }

    lua_getglobal(self->L, func_name);
    int is_func = lua_isfunction(self->L, -1);
    lua_pop(self->L, 1);

    if (is_func) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyMethodDef LuaVM_methods[] = {
    {"execute", (PyCFunction)LuaVM_execute, METH_VARARGS, "Execute a Lua script"},
    {"call", (PyCFunction)LuaVM_call, METH_VARARGS, "Call a global Lua function"},
    {"function_exists", (PyCFunction)LuaVM_function_exists, METH_VARARGS, "Check if a global Lua function exists"},
    {NULL}
};

static PyTypeObject LuaVMType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pylua.LuaVM",
    .tp_doc = "Lua Virtual Machine",
    .tp_basicsize = sizeof(LuaVM),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)LuaVM_init,
    .tp_dealloc = (destructor)LuaVM_dealloc,
    .tp_methods = LuaVM_methods,
};

static struct PyModuleDef pyluamodule = {
    PyModuleDef_HEAD_INIT,
    "_luaward",
    "Python interface to Lua",
    -1,
    NULL
};

PyMODINIT_FUNC PyInit__luaward(void) {
    PyObject *m;
    if (PyType_Ready(&LuaVMType) < 0)
        return NULL;

    m = PyModule_Create(&pyluamodule);
    if (m == NULL)
        return NULL;

    Py_INCREF(&LuaVMType);
    if (PyModule_AddObject(m, "LuaVM", (PyObject *)&LuaVMType) < 0) {
        Py_DECREF(&LuaVMType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
