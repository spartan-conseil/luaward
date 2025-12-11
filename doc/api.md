# API Reference

## `IsolatedLuaVM`

The primary class for interacting with LuaWard.

```python
from luaward.isolated import IsolatedLuaVM
```

### Constructor

```python
def __init__(self, 
             memory_limit=None, 
             callbacks=None, 
             instruction_limit=None, 
             uid=None, 
             gid=None, 
             full_isolation=False,
             cpu_limit=None)
```

**Parameters:**

*   `memory_limit` (int, optional): RAM limit for the Lua VM in bytes. Default: Unlimited (or C default, ~5MB).
*   `instruction_limit` (int, optional): Maximum number of Lua instructions allowed before interruption. Useful for stopping infinite loops.
*   `callbacks` (dict, optional): Dictionary `{ "lua_name": python_function }` exposing Python functions to Lua.
*   `uid` (int, optional): User ID under which the worker process should run (requires root or sudo initially).
*   `gid` (int, optional): Group ID for the worker process.
*   `full_isolation` (bool, default `False`): Enables advanced isolation (Network Namespace, Seccomp). Recommended for production.
*   `cpu_limit` (int, optional): Maximum CPU time in seconds (RLIMIT_CPU) before the OS kills the worker process.

### Methods

#### `execute(script: str)`

Executes a complete Lua script.

*   **Arguments**: `script` (str) - The Lua source code.
*   **Returns**: Nothing (`None`) on success.
*   **Raises**: `RuntimeError` on Lua error, `TimeoutError` if time/instruction limit is exceeded.

#### `call(func_name: str, *args)`

Calls a global Lua function with arguments.

*   **Arguments**:
    *   `func_name` (str): Name of the global function.
    *   `*args`: Arguments to pass (automatically converted from Python to Lua).
*   **Returns**: The return value of the Lua function (converted to Python type).

#### `function_exists(func_name: str) -> bool`

Checks if a global Lua function exists.

#### `close()`

Cleanly terminates the worker process and releases resources.

## Complete Example

```python
from luaward.isolated import IsolatedLuaVM

def py_log(msg):
    print(f"[PYTHON LOG] {msg}")

# Secure initialization
vm = IsolatedLuaVM(
    memory_limit=1024 * 1024, # 1 MB
    instruction_limit=10000,
    full_isolation=True,
    callbacks={'log': py_log}
)

try:
    # Code execution
    vm.execute("""
        local x = 10
        local y = 20
        log("Calculation in progress...") -- Python callback call
        result = x + y
    """)
    
    # Retrieving result via call
    res = vm.call("tostring", 30) # We can call exposed standard functions
    print(f"Result: {res}")
    
except Exception as e:
    print(f"Error: {e}")
finally:
    vm.close()
```
