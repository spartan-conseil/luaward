import unittest
import time
from luaward import IsolatedLuaVM

class TestInstructionLimit(unittest.TestCase):
    def test_no_limit(self):
        # Default should have no limit
        vm = IsolatedLuaVM()
        # A simple loop
        vm.execute("local x = 0; for i = 1, 100000 do x = x + 1 end")
        # Should succeed
        vm.close()

    def test_limit_exceeded(self):
        # Set a small limit (e.g. 1000 instructions)
        # Note: The granularity is 1000, so we should set something slightly larger or equal
        # But since we check every 1000 instructions, limit=1000 might trigger right after 1000.
        vm = IsolatedLuaVM(instruction_limit=2000)
        
        script = """
        local x = 0
        while true do
            x = x + 1
        end
        """
        
        with self.assertRaises(RuntimeError) as cm:
            vm.execute(script)
        
        self.assertIn("Instruction limit exceeded", str(cm.exception))
        vm.close()

    def test_limit_not_exceeded(self):
        # Limit 50000, loop 10000
        # 10000 iterations * approx 3-4 instructions per iter = 30k-40k instructions
        # It's close, let's make the loop smaller to be safe.
        # 1000 iters * 4 = 4000 instr. Limit 10000.
        vm = IsolatedLuaVM(instruction_limit=20000)
        vm.execute("local x = 0; for i = 1, 1000 do x = x + 1 end")
        # Should succeed
        vm.close()

    def test_limit_reset(self):
        # Verify limit resets between calls
        vm = IsolatedLuaVM(instruction_limit=20000)
        
        # Run 1
        vm.execute("local x = 0; for i = 1, 1000 do x = x + 1 end")
        
        # Run 2 (should not accumulate count from Run 1)
        vm.execute("local x = 0; for i = 1, 1000 do x = x + 1 end")
        
        vm.close()

if __name__ == '__main__':
    unittest.main()
