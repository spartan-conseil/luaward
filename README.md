# LuaWard

**Secure, Isolated, and Interactive Lua Binding for Python**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python 3](https://img.shields.io/badge/python-3.7+-blue.svg)](https://www.python.org/downloads/)
[![Lua 5.4](https://img.shields.io/badge/lua-5.4-000080.svg)](https://www.lua.org/)

LuaWard allows you to execute Lua scripts from Python in a **secure**, **process-isolated**, and **memory-limited** environment. It is designed for applications that need to run untrusted user scripts safely.

## Features

- 🛡️ **Process Isolation**: The Lua VM runs in a completely separate process. A crash in Lua will not bring down your Python application.
- 📦 **Sandboxing**: Dangerous standard libraries (`os`, `io`, `debug`, etc.) are removed by default. Only safe libraries (`base`, `string`, `table`, `math`) are available.
- 💾 **Memory Limits**: Enforce strict memory usage limits (e.g., 5MB). The VM terminates if the script exceeds the quota.
- 🔄 **Bi-directional Communication**:
  - Call Python functions from Lua via registered callbacks.
  - Call global Lua functions from Python.
  - Check for function existence before calling.

## Installation

LuaWard must be installed from source. You will need a C compiler (gcc) installed.

### From Source

Clone the repository and install using pip:

```bash
git clone https://github.com/luaward/luaward.git
cd luaward
pip install .
```

For development (editable mode):

```bash
pip install -e .
```

### Using Makefile

You can also use the provided Makefile:

```bash
make install
```

## Usage

### Basic Execution

```python
from isolated_lua_vm import IsolatedLuaVM

# Create a VM with a 10MB memory limit
vm = IsolatedLuaVM(memory_limit=10 * 1024 * 1024)

try:
    # Execute a script
    vm.execute('print("Hello from Lua!")')
    
    # Calculate something
    vm.execute('result = 10 + 20')
except Exception as e:
    print(f"Lua Error: {e}")
finally:
    vm.close()
```

### Calling Lua Functions

```python
vm.execute("""
function greet(name)
    return "Hello, " .. name
end
""")

message = vm.call("greet", "Python User")
print(message)  # Output: Hello, Python User
```

### Callbacks (Python from Lua)

You can expose Python functions to the Lua environment.

```python
def my_algo(x):
    return x * 2

callbacks = {
    "double_it": my_algo
}

vm = IsolatedLuaVM(callbacks=callbacks)
vm.execute('res = double_it(21)')
# res is now 42 inside Lua
```

### Checking Function Existence

```python
if vm.function_exists("suspicious_func"):
    vm.call("suspicious_func")
else:
    print("Function not found, skipping.")
```

## Security & Limitations

- **No filesystem access**: `io` and `os` libraries are stripped.
- **No external modules**: `require`, `module`, and `package` are stripped.
- **Single Threaded**: The Lua VM runs in a single thread within its worker process.
- **Data Types**: Supports conversion of basic types (`number`, `string`, `boolean`, `nil`). Tables are not automatically converted to dicts/lists in this version.

## License

This project is licensed under the MIT License.
