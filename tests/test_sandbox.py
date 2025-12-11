import unittest
from luaward import IsolatedLuaVM

class TestSandbox(unittest.TestCase):
    def setUp(self):
        self.vm = IsolatedLuaVM()

    def tearDown(self):
        self.vm.close()

    def test_basic_whitelist(self):
        # Should work
        self.vm.execute("assert(true)")
        self.vm.execute("print('hello')")
        self.vm.execute("local t = {1, 2}; table.insert(t, 3)")
        self.vm.execute("local x = math.abs(-10)")
        self.vm.execute("local s = string.upper('abc')")

    def test_dangerous_functions_removed(self):
        dangerous = [
            "os", "io", "debug", "package", "coroutine",
            "dofile", "load", "loadfile", "require", "module", "collectgarbage",
            "getmetatable", "setmetatable", "rawget", "rawset", "rawequal"
        ]
        
        for func in dangerous:
            # Check global existence
            # Note: libraries like os, io should be nil
            check = f"if {func} ~= nil then error('{func} should be nil') end"
            self.vm.execute(check)

    def test_filtered_libraries(self):
        # string.dump should be missing
        script = """
        if string.dump then error('string.dump should be missing') end
        """
        self.vm.execute(script)

        # math.random should be present
        self.vm.execute("if not math.random then error('math.random missing') end")

    def test_string_metatable_protection(self):
        # Verify (""):dump() is not possible
        script = """
        local status, err = pcall(function() return (""):dump() end)
        if status then error('dump() on string should fail') end
        """
        self.vm.execute(script)

        # Verify normal string methods work
        self.vm.execute("assert(('abc'):upper() == 'ABC')")

if __name__ == '__main__':
    unittest.main()
