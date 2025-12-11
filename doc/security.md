# Security

Security is the top priority of LuaWard. It is ensured by a "Defense in Depth" approach, combining several layers of protection.

## 1. Lua Sandbox (Application Level)

The first level of defense is the Lua environment itself.

*   **Empty Environment**: The VM starts with a nearly empty global environment.
*   **Whitelist**: Only safe functions are exposed.
    *   `print`, `pairs`, `ipairs`, `next`, `tonumber`, `tostring`, `type`, `error`, `assert`, `pcall`, `xpcall`, `select`.
*   **Filtered Libraries**: Standard libraries are sanitized.
    *   **Removed**: `io`, `os`, `package`, `coroutine`, `debug`, `dofile`, `load`, `loadfile`, `require`.
    *   **Restricted**:
        *   `math`: (Pure mathematical functions only).
        *   `string`: (String manipulation only).
        *   `table`: (Table manipulation only).
*   **Metatable Protection**: Metatables of basic types (like `string`) are modified to point to the filtered versions of libraries, preventing access to dangerous functions via object methods (e.g., `("cmd"):execute()` is impossible).

## 2. Resource Quotas (VM Level)

These protections prevent Denial of Service (DoS) attacks that consume too many resources.

*   **Memory Limit**:
    *   Implemented via a custom allocator (`lua_setallocf`).
    *   Tracks exactly every byte allocated by the Lua VM.
    *   Blocks any allocation exceeding the defined limit (default 5 MB).
    *   Protects against memory leaks and "Memory Exhaustion Attacks".
*   **Instruction Limit (CPU Time Limit)**:
    *   Implemented via Lua hooks (`lua_sethook`).
    *   Counts the number of bytecode instructions executed.
    *   Interrupts execution if the limit is exceeded (e.g., infinite loops).

## 3. Process Isolation (OS Level)

Lua code runs in a separate process (`IsolatedLuaVM`), isolated from the main process.

*   **Network Isolation**:
    *   `unshare(CLONE_NEWNET)` syscall.
    *   The process ends up in an empty network namespace. It has no access to any network interface, preventing any external communication (c2 channel, data exfiltration).
*   **Privilege Dropping (User/Group Dropping)**:
    *   Ability to define a specific UID and GID for the worker.
    *   Allows code execution as a "nobody" or dedicated user, without write rights on the file system.
*   **System Limits (rlimit)**:
    *   `RLIMIT_CPU`: Maximum CPU time (hardkill protection against native infinite loops).
    *   `RLIMIT_AS`: Maximum virtual address space.

## 4. System Call Filtering (Seccomp)

This is the ultimate protection at the Linux kernel level ("Lockdown").

*   **Mode**: `SECCOMP_MODE_FILTER`.
*   **Policy**: Strict whitelist (Everything not allowed is forbidden and kills the process `SECCOMP_RET_KILL`).
*   **Explicitly Forbidden Syscalls**:
    *   `execve`, `execveat`: Prevents execution of any new program (e.g., `/bin/sh`).
    *   `fork`, `vfork`, `clone`: Prevents creation of new processes.
    *   `socket`, `connect`, `bind`, `accept`, `listen`: Prevents any low-level network operation.
    *   `ptrace`: Prevents inspection/injection of code into other processes.
*   **Note**: Seccomp is enabled via the `full_isolation=True` option.

## Security Summary

| Threat | Protection |
| :--- | :--- |
| **Arbitrary Code Execution** | Lua Sandbox (Whitelist) |
| **File System Access** | Lua Sandbox (No IO) + UID Dropping |
| **Network Access** | Lua Sandbox (No Socket) + Network Namespace + Seccomp |
| **Denial of Service (CPU)** | Instruction Limit + RLIMIT_CPU |
| **Denial of Service (RAM)** | Memory Limit (Allocator) |
| **Privilege Escalation** | UID/GID low privilege + Seccomp |
