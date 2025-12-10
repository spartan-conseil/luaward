import multiprocessing
import queue
import sys
import io
import contextlib
import _luaward

class IsolatedLuaVM:
    def __init__(self, memory_limit=None, callbacks=None):
        self.cmd_queue = multiprocessing.Queue()
        self.result_queue = multiprocessing.Queue()
        
        # Store callbacks locally to execute them on request
        self.callbacks = callbacks or {}
        callback_names = list(self.callbacks.keys())

        self.process = multiprocessing.Process(
            target=self._worker_loop,
            args=(self.cmd_queue, self.result_queue, memory_limit, callback_names)
        )
        self.process.start()

    def _worker_loop(self, cmd_q, res_q, mem_limit, callback_names):
        # Create proxy functions for each callback name
        proxies = {}
        for name in callback_names:
            def make_proxy(func_name):
                def proxy(*args):
                    res_q.put(('CALLBACK', (func_name, args)))
                    # Wait for response
                    while True:
                        try:
                            cmd, payload = cmd_q.get()
                            if cmd == 'CALLBACK_RESULT':
                                return payload
                            elif cmd == 'STOP':
                                raise SystemExit("Worker stopped during callback")
                        except Exception:
                            raise
                return proxy
            proxies[name] = make_proxy(name)

        try:
            vm = _luaward.LuaVM(memory_limit=mem_limit, callbacks=proxies) if mem_limit else _luaward.LuaVM(callbacks=proxies)
        except Exception as e:
            res_q.put(('CRITICAL', f"Init failed: {e}"))
            return

        while True:
            try:
                cmd, payload = cmd_q.get()
                if cmd == 'STOP':
                    break
                elif cmd == 'EXECUTE':
                    try:
                        vm.execute(payload)
                        res_q.put(('SUCCESS', None))
                    except Exception as e:
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'CALL':
                    func_name, args = payload
                    try:
                        res = vm.call(func_name, *args)
                        res_q.put(('SUCCESS', res))
                    except Exception as e:
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'FUNCTION_EXISTS':
                    func_name = payload
                    try:
                        exists = vm.function_exists(func_name)
                        res_q.put(('SUCCESS', exists))
                    except Exception as e:
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'CALLBACK_RESULT':
                    pass # Unexpected here
            except SystemExit:
                break
            except Exception as e:
                res_q.put(('CRITICAL', str(e)))
                break

    def _wait_for_result(self, send_callback=None):
        # send_callback arg is deprecated/removed in favor of self.callbacks
        while True:
            status, payload = self.result_queue.get()
            if status == 'SUCCESS':
                return payload
            elif status == 'ERROR':
                raise RuntimeError(payload)
            elif status == 'CRITICAL':
                raise SystemError(f"Worker crashed: {payload}")
            elif status == 'CALLBACK':
                # payload is (func_name, args)
                func_name, args = payload
                if func_name in self.callbacks:
                    try:
                        # Execute callback with unpacked args
                        response = self.callbacks[func_name](*args)
                        self.cmd_queue.put(('CALLBACK_RESULT', response))
                    except Exception as e:
                        self.cmd_queue.put(('CALLBACK_RESULT', f"Error in callback {func_name}: {e}"))
                else:
                     self.cmd_queue.put(('CALLBACK_RESULT', f"Callback '{func_name}' not found"))
            else:
                 raise ValueError(f"Unknown status: {status}")

    def execute(self, script):
        """
        Executes script.
        """
        self.cmd_queue.put(('EXECUTE', script))
        return self._wait_for_result()

    def call(self, func_name, *args):
        """
        Calls a global Lua function with arguments.
        """
        self.cmd_queue.put(('CALL', (func_name, args)))
        return self._wait_for_result()

    def function_exists(self, func_name):
        """
        Checks if a global Lua function exists.
        """
        self.cmd_queue.put(('FUNCTION_EXISTS', func_name))
        return self._wait_for_result()

    def close(self):
        self.cmd_queue.put(('STOP', None))
        self.process.join()
