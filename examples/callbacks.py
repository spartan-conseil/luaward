from luaward import IsolatedLuaVM

def py_reverse(s):
    """Reverses a string."""
    return s[::-1]

def py_max(a, b):
    """Returns max of two numbers."""
    return max(a, b)

def main():
    print("--- Callbacks Example ---")
    
    callbacks = {
        "reverse_string": py_reverse,
        "max_val": py_max
    }
    
    vm = IsolatedLuaVM(callbacks=callbacks)
    
    try:
        # Call Python from Lua
        script = """
        s = "LuaWard"
        rev = reverse_string(s)
        print("Original: " .. s)
        print("Reversed (via Python): " .. rev)
        
        m = max_val(10, 42)
        print("Max value (via Python): " .. m)
        """
        vm.execute(script)
        
    finally:
        vm.close()

if __name__ == "__main__":
    main()
