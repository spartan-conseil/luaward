# LuaWard Architecture

LuaWard is designed to execute untrusted Lua code securely and in isolation within a Python application.

## Core Components

The architecture relies on two main components:

1.  **The C Extension (`_luaward`)**: The core of execution and security enforcement.
2.  **The Python Wrapper (`IsolatedLuaVM`)**: The process management and system isolation layer.

### 1. C Extension (`_luaward`)

The C extension is a Python binding to the Lua 5.4 library. It is responsible for:

*   **Lua VM Creation**: Initializing a pristine Lua state (`lua_State`).
*   **Memory Management**: Using a custom memory allocator (`l_alloc`) to enforce strict limits (Memory Quota).
*   **Execution Control**: Using Lua hooks to count instructions and interrupt execution if a limit is reached (Instruction Limit).
*   **Sandbox**: Initializing a restricted Lua environment by removing dangerous libraries (`io`, `os`, `package`, etc.) and filtering safe ones (`string`, `table`, `math`).
*   **Python/Lua Interoperability**: Transparent type conversion (None, bool, int, float, str) and Python callback management.
*   **Seccomp**: Applying `seccomp` (Secure Computing Mode) filters to restrict allowed system calls at the Linux kernel level.

### 2. Python Wrapper (`IsolatedLuaVM`)

This class (in `luaward.isolated.py`) encapsulates the C extension and provides process-level isolation:

*   **Process Isolation**: Each Lua VM runs in its own subprocess (`multiprocessing.Process`), totally isolating memory from the main Python interpreter.
*   **Network Isolation**: Uses `unshare(CLONE_NEWNET)` to detach the process from the network (it sees no network interfaces except `lo` which is down).
*   **Resource Limits**: Uses `resource.setrlimit` (RLIMIT_AS, RLIMIT_CPU) to limit the global consumption of the process.
*   **Privilege Dropping**: Changes UID/GID via `os.setuid`/`os.setgid` to execute code as an unprivileged user.
*   **IPC Communication**: Uses `multiprocessing.Queue` to exchange commands (`EXECUTE`, `CALL`) and results between the main process and the worker.

## Execution Flow

1.  **Initialization**: The user instantiates `IsolatedLuaVM`. The worker process starts.
2.  **Configuration**: The worker applies restrictions (Network, UID/GID, Seccomp) and initializes `_luaward.LuaVM`.
3.  **Command Loop**: The worker waits for commands on the `cmd_queue`.
4.  **Execution**:
    *   The main process sends a command (e.g., `("EXECUTE", "print('hello')")`).
    *   The worker receives it, executes via `_luaward`, and sends back the result or error via `result_queue`.
    *   If the Lua script calls a Python callback, the worker sends a `CALLBACK` request to the parent process, waits for the response, and returns it to Lua.
5.  **Termination**: Calling `vm.close()` sends a stop signal; the worker terminates cleanly.
