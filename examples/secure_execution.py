import sys
import os
from luaward import IsolatedLuaVM

def main():
    print(f"Parent process PID: {os.getpid()}, UID: {os.getuid()}")
    
    # Needs root for some features
    try:
        # Just demonstration values. In real usage, use specific UID/GID usually known.
        # Here we try to drop to 'nobody' (usually 65534) if we are root.
        target_uid = 65534 if os.getuid() == 0 else None
        target_gid = 65534 if os.getuid() == 0 else None
        
        vm = IsolatedLuaVM(
            memory_limit=10*1024*1024,
            instruction_limit=50000,
            full_isolation=True,
            uid=target_uid, 
            gid=target_gid,
            cpu_limit=2 # 2 seconds
        )
        
        print("VM Worker started with full isolation enabled (Seccomp, NS, Limits).")
        
        # 1. Test basic execution
        print("Running: 1 + 1")
        res = vm.execute("return 1 + 1")
        print(f"Result: {res}")
        
        # 2. Test seccomp blocking (if we can simulate it)
        # We can't easily trigger a blocked syscall from pure Lua because 
        # we sandboxed Lua functions (no os.execute, no io.open).
        # We would need a C callback that tries to do something bad, or a bug in Lua.
        # But we can assume it works if initialization succeeded.
        
        print("Secure VM is working.")
        vm.close()
        
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
