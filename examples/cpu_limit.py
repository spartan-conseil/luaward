from luaward import IsolatedLuaVM

def main():
    print("Initializing LuaVM with instruction limit of 100,000...")
    # Initialize VM with 100k instruction limit
    vm = IsolatedLuaVM(instruction_limit=100000)

    print("\n1. Running a safe script (simple arithmetic)...")
    try:
        vm.execute("local x = 0; for i = 1, 1000 do x = x + 1 end")
        print("Success!")
    except Exception as e:
        print(f"Failed unexpectedly: {e}")

    print("\n2. Running an expensive script (infinite loop)...")
    try:
        # infinite loop
        vm.execute("while true do end")
    except Exception as e:
        print(f"Caught expected error: {e}")

    # Show it's still usable
    print("\n3. Running safe script again to prove VM is still alive...")
    try:
        vm.execute("return 'alive'")
        print("VM is alive!")
    except Exception as e:
        print(f"VM died: {e}")

    vm.close()

if __name__ == "__main__":
    main()
