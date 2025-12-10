import unittest
from luaward import IsolatedLuaVM

class TestBasicFunctionality(unittest.TestCase):
    def setUp(self):
        self.callbacks = {
            "ping": lambda msg: f"pong: {msg}",
            "add": lambda a, b: a + b
        }
        self.vm = IsolatedLuaVM(memory_limit=5*1024*1024, callbacks=self.callbacks)

    def tearDown(self):
        self.vm.close()

    def test_execution(self):
        """Test simple script execution"""
        result = self.vm.execute('print("Hello from unittest")')
        self.assertIsNone(result)

    def test_callback(self):
        """Test generic callback functionality"""
        script = """
        res = ping("hello")
        if res ~= "pong: hello" then error("ping failed") end
        
        val = add(10, 20)
        if val ~= 30 then error("add failed: "..val) end
        """
        result = self.vm.execute(script)
        self.assertIsNone(result)

    def test_call_method(self):
        """Test calling Lua function from Python"""
        self.vm.execute("""
        function greet(name) return "Hello " .. name end
        function sum(a, b) return a + b end
        """)
        
        res = self.vm.call("greet", "World")
        self.assertEqual(res, "Hello World")
        
        res = self.vm.call("sum", 5, 7)
        self.assertEqual(res, 12)

    def test_call_python_from_lua_call(self):
        """Test calling Python callback inside a Lua function called from Python context"""
        self.vm.execute("""
        function call_ping(msg) 
            return ping(msg)
        end
        """)
        res = self.vm.call("call_ping", "unittest")
        self.assertEqual(res, "pong: unittest")

    def test_function_exists(self):
        """Test checking if a function exists"""
        self.vm.execute("""
        function my_func() end
        my_var = 10
        """)
        self.assertTrue(self.vm.function_exists("my_func"))
        self.assertFalse(self.vm.function_exists("non_existent_func"))
        self.assertFalse(self.vm.function_exists("my_var")) # It's a number, not a function

    def test_missing_function_call(self):
        """Test calling a non-existent function"""
        with self.assertRaises(RuntimeError) as cm:
            self.vm.call("ghost_function", 1, 2)
        
        self.assertIn("not a function", str(cm.exception))
