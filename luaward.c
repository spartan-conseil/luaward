#include <Python.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "structmember.h"
#include <stddef.h> /* for offsetof */
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <errno.h>

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
    // Method: Load libraries without registering them globally (glb=0)
    // Then assume control of _G.

    // 1. Basic library
    // We can't avoid loading some basic stuff into _G because luaopen_base does it.
    // However, we can clear _G afterwards and only put back what we want.
    // Actually, luaopen_base sets _G fields directly.
    
    // Strategy: Create a fresh environment table, populate it, and set it as _G?
    // Or just clean up _G.
    
    // Let's rely on manual population.
    
    // Initialize standard _G (base) to get basics, then we strip it.
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);

    // List of allowed globals (from base)
    const char *base_whitelist[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "print", 
        "select", "tonumber", "tostring", "type", "xpcall", "_VERSION", NULL
    };
    
    // Allowlist approach:
    // Create new table, copy allowlisted items from _G to it, then replace _G?
    // Or easier: Iterate over _G and remove anything not in whitelist.
    
    lua_pushglobaltable(L); // Push _G
    lua_pushnil(L);         // First key
    while (lua_next(L, -2) != 0) {
        // key at -2, value at -1
        // We only care if key is string
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *key = lua_tostring(L, -2);
            int kept = 0;
            for (int i = 0; base_whitelist[i] != NULL; i++) {
                if (strcmp(key, base_whitelist[i]) == 0) {
                    kept = 1;
                    break;
                }
            }
            if (!kept) {
                 // Remove it: _G[key] = nil
                 // We can't do it while iterating easily. 
                 // Actually we can, but it is risky in some languages. In Lua it is generally safe during next?
                 // Safer: Build a list of keys to remove.
            }
        }
        lua_pop(L, 1); // Pop value
    }
    lua_pop(L, 1); // Pop _G

    // BETTER STRATEGY: 
    // 1. Create a "sandbox" table.
    // 2. Populate it.
    // 3. Set it as _G.

    // But luaopen_base writes to the global table directly. 
    // So let's let it run, then we SCRUB the global table.
    
    // To Scrub:
    // We will construct a list of keys to KEEP. 
    // Then we clear everything else. 
    // Wait, clearing everything else is hard if we don't know what's there.
    // WE DO know what's there: whatever luaopen_base put + pre-existing?
    
    // Even Better:
    // 1. lua_newtable(L) -> sandbox
    // 2. Open libs into it.
    
    // Actually, simple scrub:
    // Remove known dangerous globals explicitly first.
    // "dofile", "load", "loadfile", "require", "module", "collectgarbage", "getmetatable", "setmetatable", "rawequal", "rawget", "rawlen", "rawset"
    const char *blacklist[] = {
        "dofile", "load", "loadfile", "require", "module", "collectgarbage", 
        "getmetatable", "setmetatable", "rawequal", "rawget", "rawlen", "rawset",
        "io", "os", "debug", "package", "coroutine", 
        NULL
    };

    for (int i = 0; blacklist[i] != NULL; i++) {
        lua_pushnil(L);
        lua_setglobal(L, blacklist[i]);
    }

    // Now load libraries into their namespaces manually to ensure we control content.
    
    // Helper to load lib and filter
    void load_lib_filtered(lua_State *L, const char *libname, lua_CFunction openf, const char **whitelist) {
        luaL_requiref(L, libname, openf, 0); // push lib to stack, don't set global
        // Stack: [lib_table]
        
        // Create new table for the filtered lib
        lua_newtable(L);
        // Stack: [lib_table, filtered_table]
        
        for (int i = 0; whitelist[i] != NULL; i++) {
            lua_getfield(L, -2, whitelist[i]); // Get from lib_table
            // Stack: [lib_table, filtered_table, func]
            if (!lua_isnil(L, -1)) {
                lua_setfield(L, -2, whitelist[i]); // Set to filtered_table
            } else {
                lua_pop(L, 1); // Pop nil
            }
        }
        
        // Remove the full lib table
        lua_remove(L, -2); // Stack: [filtered_table]
        
        // Set it as global variable
        lua_setglobal(L, libname);
    }
    
    const char *table_whitelist[] = {"concat", "insert", "move", "pack", "remove", "sort", "unpack", NULL};
    load_lib_filtered(L, "table", luaopen_table, table_whitelist);
    
    const char *string_whitelist[] = {
        "byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", 
        "match", "rep", "reverse", "sub", "upper", NULL
    };
    load_lib_filtered(L, "string", luaopen_string, string_whitelist);
    
    const char *math_whitelist[] = {
        "abs", "acos", "asin", "atan", "ceil", "cos", "deg", "exp", "floor", 
        "fmod", "huge", "log", "max", "min", "modf", "pi", "rad", "random", 
        "randomseed", "sin", "sqrt", "tan", "tointeger", "type", "ult", NULL
    };
    load_lib_filtered(L, "math", luaopen_math, math_whitelist);
    
    const char *utf8_whitelist[] = {"char", "codes", "codepoint", "len", "offset", NULL};
    load_lib_filtered(L, "utf8", luaopen_utf8, utf8_whitelist);

    // Filter _G (base) explicitly again to be sure?
    // We already did blacklist. Let's do whitelist on _G too?
    // It's cleaner to blacklist the base lib dangerous functions because base lib puts them in root.
    // Whitelist for _G is hard because it contains _G itself, etc.
    // The blacklist above covers the dangerous base functions.
    // Plus we haven't loaded io, os, debug at all.
    
    // One more thing: string metatable. 
    // Strings in Lua have a metatable pointing to string library.
    // luaopen_string sets this. 
    // Since we filtered "string" global, scripts can't access full string lib via 'string'.
    // BUT they can do ("foo"):dump() if the metatable points to the ORIGINAL string lib!
    // Lua 5.4 luaopen_string sets the string metatable to the table it returns.
    // The table it returns is the FULL table.
    // So we must fix the string metatable to point to our filtered table.
    
    lua_getglobal(L, "string"); // Our filtered table
    lua_pushstring(L, "");      // A string
    lua_getmetatable(L, -1);    // The current metatable (the full lib) or nil
    if (!lua_istable(L, -1)) {
        // Create one if missing (unlikely)
        lua_pop(L, 1);
        lua_createtable(L, 0, 0); 
    }
    // Actually we want to SET `__index` of the string metatable to our filtered lib.
    // Wait, the string library IS the `__index` of the string metatable usually.
    
    // Let's protect strings:
    lua_pushstring(L, "");
    if (lua_getmetatable(L, -1)) { // Pushes metatable
        lua_getglobal(L, "string"); // Pushes filtered string lib
        lua_setfield(L, -2, "__index"); // mt.__index = filtered_string
        lua_pop(L, 2); // pop string and metatable
    } else {
         lua_pop(L, 1); // pop string
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

static int install_seccomp(void) {
    struct sock_filter filter[] = {
        /* Validate architecture to be x86_64 */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

        /* Load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, nr))),

        /* Denylist dangerous syscalls */
        /* Execve */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        
        /* Execveat */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),

        /* Fork / Clone / Vfork - BE CAREFUL. Python might need clone for threads. 
           But this is an isolated process. 
           Let's block fork/vfork, but maybe allow clone if it's thread creation? 
           For strict isolation we block new process creation. 
        */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fork, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),

        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_vfork, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        
        /* Socket */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        
        /* Connect */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        
        /* Bind */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),

        /* Accept */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        
        /* Ptrace */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ptrace, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),

        /* Allow everything else */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        return -1;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        return -1;
    }
    
    return 0;
}

static PyObject *luaward_lockdown(PyObject *self, PyObject *args) {
    if (install_seccomp() < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"lockdown", luaward_lockdown, METH_NOARGS, "Apply seccomp filter to current process"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef pyluamodule = {
    PyModuleDef_HEAD_INIT,
    "_luaward",
    "Python interface to Lua",
    -1,
    module_methods
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
