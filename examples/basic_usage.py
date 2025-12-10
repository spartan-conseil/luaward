from luaward import IsolatedLuaVM

def main():
    print("--- Basic Usage Example ---")
    
    # Initialize VM with 5MB memory limit
    vm = IsolatedLuaVM(memory_limit=5 * 1024 * 1024)
    
    try:
        # Execute simple script
        print("Executing script...")
        vm.execute('print("Hello from Lua!")')
        vm.execute('x = 100; y = 200')
        vm.execute('print("Sum inside Lua: " .. (x + y))')
        
        # Call a Lua function
        print("\nCalling function...")
        vm.execute("""
        function multiply(a, b)
            return a * b
        end
        """)
        
        result = vm.call("multiply", 6, 7)
        print(f"Result of 6 * 7 calculated in Lua: {result}")
        
    finally:
        vm.close()
        print("\nVM Closed.")

if __name__ == "__main__":
    main()
