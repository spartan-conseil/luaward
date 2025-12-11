# LuaWard Documentation

Welcome to the official LuaWard documentation.

LuaWard is a Python library that allows secure and isolated execution of Lua code. It is designed for applications requiring the execution of untrusted scripts (plugins, modding, user scripts) without compromising host system security.

## Table of Contents

1.  [Architecture](architecture.md): Understand how LuaWard works under the hood.
2.  [Security](security.md): Detail of protection mechanisms (Sandbox, CGroups, Seccomp, etc.).
3.  [API Reference](api.md): Documentation of the `IsolatedLuaVM` class.

## Installation

```bash
pip install luaward
```

*Note: LuaWard requires an x86_64 Linux system to benefit from all security features (Seccomp, Namespaces).*

## Quick Start

```python
from luaward.isolated import IsolatedLuaVM

# Create an isolated VM
vm = IsolatedLuaVM(memory_limit=1024*1024)

# Execute Lua code
vm.execute("print('Hello from Lua!')")

# Clean up
vm.close()
```

For more details, consult the [API Reference](api.md).
