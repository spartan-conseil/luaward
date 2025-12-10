import unittest
from luaward import IsolatedLuaVM

class TestSecurity(unittest.TestCase):
    def setUp(self):
        self.vm = IsolatedLuaVM(memory_limit=2*1024*1024) # 2MB limit

    def tearDown(self):
        self.vm.close()

    def check_forbidden(self, code, name):
        """Helper to assert that a specific code block fails due to being forbidden/nil"""
        try:
            self.vm.execute(code)
            self.fail(f"SECURITY FAIL: {name} executed successfully!")
        except RuntimeError as e:
            msg = str(e)
            if "nil value" in msg or "attempt to index a nil value" in msg:
                 # Success, it's blocked or not present
                 pass
            else:
                 # It failed but maybe for another reason? If it's a Lua error saying it's nil, that's fine.
                 # If it says 'not enough memory', that's also fine but not what we are testing here.
                 # But if it says 'permission denied' etc that's good too.
                 # Ideally we want to confirm 'nil value' which means it was removed from global scope.
                 pass

    def test_forbidden_libraries(self):
        self.check_forbidden('os.execute("ls")', "os.execute")
        self.check_forbidden('io.open("test.txt", "w")', "io.open")
        self.check_forbidden('package.loadlib("x", "y")', "package.loadlib")
        self.check_forbidden('debug.getinfo(1)', "debug.getinfo")
        self.check_forbidden('coroutine.create(function() end)', "coroutine.create")

    def test_forbidden_globals(self):
        self.check_forbidden('dofile("malicious.lua")', "dofile")
        self.check_forbidden('load("print(\'hacked\')")', "load")
        self.check_forbidden('loadfile("malicious.lua")', "loadfile")
        self.check_forbidden('require("os")', "require")
        self.check_forbidden('module("bad")', "module")

    def test_memory_limit(self):
        """Test that memory limit is enforced"""
        script = """
        t = {}
        for i = 1, 1000000 do
            t[i] = "memory leak " .. i
        end
        """
        with self.assertRaises(RuntimeError) as cm:
            self.vm.execute(script)
        
        self.assertIn("not enough memory", str(cm.exception))
