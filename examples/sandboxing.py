from luaward import IsolatedLuaVM

def main():
    print("--- Sandboxing Example ---")
    vm = IsolatedLuaVM()
    
    try:
        print("Attempting to access 'os' library (should fail)...")
        # os.execute would let us run shell commands - strictly forbidden
        vm.execute('os.execute("ls")')
        print("CRITICAL: os.execute worked! (This should not happen)")
    except RuntimeError as e:
        print(f"Success! Blocked dangerous call: {e}")
        
    try:
        print("\nAttempting to read a file (should fail)...")
        # io.open allows reading files
        vm.execute('f = io.open("/etc/passwd", "r")')
        print("CRITICAL: io.open worked!")
    except RuntimeError as e:
         print(f"Success! Blocked filesystem access: {e}")

    vm.close()

if __name__ == "__main__":
    main()
