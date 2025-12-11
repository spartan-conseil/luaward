import multiprocessing
import queue
import sys
import io
import contextlib
import os
import ctypes
import resource
import _luaward

class IsolatedLuaVM:
    def __init__(self, memory_limit=None, callbacks=None, instruction_limit=None, 
                 uid=None, gid=None, full_isolation=False,
                 cpu_limit=None):
        self.cmd_queue = multiprocessing.Queue()
        self.result_queue = multiprocessing.Queue()
        
        # Store callbacks locally to execute them on request
        self.callbacks = callbacks or {}
        callback_names = list(self.callbacks.keys())

        # Limits and credentials
        self.uid = uid
        self.gid = gid
        self.full_isolation = full_isolation
        self.cpu_limit = cpu_limit # CPU time in seconds

        self.process = multiprocessing.Process(
            target=self._worker_loop,
            args=(self.cmd_queue, self.result_queue, memory_limit, 
                  callback_names, instruction_limit, 
                  self.uid, self.gid, self.full_isolation, self.cpu_limit)
        )
        self.process.start()

    def _worker_loop(self, cmd_q, res_q, mem_limit, callback_names, instruction_limit, 
                     uid, gid, full_isolation, cpu_limit):
        self._setup_logging()
        self.logger.info("Worker started")
        
        self._setup_isolation(full_isolation, cpu_limit, uid, gid)
        proxies = self._create_proxies(callback_names, cmd_q, res_q)
        
        try:
            vm = self._init_vm(mem_limit, instruction_limit, proxies)
        except Exception as e:
            self.logger.critical(f"VM Init failed: {e}")
            res_q.put(('CRITICAL', f"Init failed: {e}"))
            return

        self._command_loop(vm, cmd_q, res_q)

    def _setup_logging(self):
        import logging
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - [IsolatedLuaVM-Worker] - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger("IsolatedLuaVM-Worker")

    def _setup_isolation(self, full_isolation, cpu_limit, uid, gid):
        # 1. Network Isolation: unshare(CLONE_NEWNET)
        if full_isolation:
            self.logger.info("Applying network isolation")
            # CLONE_NEWNET = 0x40000000
            try:
                libc = ctypes.CDLL(None)
                if libc.unshare(0x40000000) != 0:
                    self.logger.warning("unshare(CLONE_NEWNET) failed (non-zero return)")
            except Exception as e:
                self.logger.warning(f"unshare(CLONE_NEWNET) failed: {e}")

        # 2. Resource Limits
        if cpu_limit:
            self.logger.info(f"Setting CPU limit to {cpu_limit}s")
            try:
                resource.setrlimit(resource.RLIMIT_CPU, (cpu_limit, cpu_limit))
            except Exception as e:
                self.logger.error(f"Failed to set CPU limit: {e}")

        # 3. Drop Privileges
        if gid is not None:
            self.logger.info(f"Dropping privileges to GID {gid}")
            try:
                os.setgid(gid)
            except Exception as e:
                self.logger.error(f"Failed to set GID: {e}")
        if uid is not None:
            self.logger.info(f"Dropping privileges to UID {uid}")
            try:
                os.setuid(uid)
            except Exception as e:
                self.logger.error(f"Failed to set UID: {e}")

        # 4. Lockdown (Seccomp)
        if full_isolation:
            self.logger.info("Applying full isolation lockdown")
            try:
                _luaward.lockdown()
            except Exception as e:
                self.logger.critical(f"Lockdown failed: {e}")
                raise # This will be caught by the caller or crash the worker, which is intended if lockdown fails

    def _create_proxies(self, callback_names, cmd_q, res_q):
        proxies = {}
        for name in callback_names:
            def make_proxy(func_name):
                def proxy(*args):
                    self.logger.debug(f"Proxy calling callback: {func_name}")
                    res_q.put(('CALLBACK', (func_name, args)))
                    # Wait for response
                    while True:
                        try:
                            cmd, payload = cmd_q.get()
                            if cmd == 'CALLBACK_RESULT':
                                return payload
                            elif cmd == 'STOP':
                                self.logger.error("Worker stopped during callback wait")
                                raise SystemExit("Worker stopped during callback")
                        except Exception as e:
                            self.logger.error(f"Error in proxy loop: {e}")
                            raise
                return proxy
            proxies[name] = make_proxy(name)
        return proxies

    def _init_vm(self, mem_limit, instruction_limit, proxies):
        self.logger.info("Initializing LuaVM")
        kwargs = {'callbacks': proxies}
        if mem_limit:
            self.logger.info(f"Memory limit: {mem_limit}")
            kwargs['memory_limit'] = mem_limit
        if instruction_limit:
            self.logger.info(f"Instruction limit: {instruction_limit}")
            kwargs['instruction_limit'] = instruction_limit
            
        return _luaward.LuaVM(**kwargs)

    def _command_loop(self, vm, cmd_q, res_q):
        self.logger.info("Entering command loop")
        while True:
            try:
                cmd, payload = cmd_q.get()
                if cmd == 'STOP':
                    self.logger.info("Received STOP command")
                    break
                elif cmd == 'EXECUTE':
                    try:
                        self.logger.debug("Executing script")
                        vm.execute(payload)
                        res_q.put(('SUCCESS', None))
                    except Exception as e:
                        self.logger.error(f"Execution error: {e}")
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'CALL':
                    func_name, args = payload
                    try:
                        self.logger.debug(f"Calling function: {func_name}")
                        res = vm.call(func_name, *args)
                        res_q.put(('SUCCESS', res))
                    except Exception as e:
                        self.logger.error(f"Call error: {e}")
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'FUNCTION_EXISTS':
                    func_name = payload
                    try:
                        exists = vm.function_exists(func_name)
                        res_q.put(('SUCCESS', exists))
                    except Exception as e:
                        self.logger.error(f"Function exists check error: {e}")
                        res_q.put(('ERROR', str(e)))
                elif cmd == 'CALLBACK_RESULT':
                    self.logger.warning("Received unexpected CALLBACK_RESULT in main loop")
                    pass 
            except SystemExit:
                self.logger.info("SystemExit in command loop")
                break
            except Exception as e:
                self.logger.critical(f"Critical error in command loop: {e}")
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
