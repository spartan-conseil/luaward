import os
import tarfile
import urllib.request
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import subprocess
import glob

LUA_VERSION = "5.4.7"
LUA_URL = f"https://www.lua.org/ftp/lua-{LUA_VERSION}.tar.gz"
LUA_DIR = f"lua-{LUA_VERSION}"
LUA_SRC_DIR = os.path.join(LUA_DIR, "src")

class BuildLuaExt(build_ext):
    def run(self):
        if not os.path.exists(LUA_DIR):
            print(f"Downloading Lua {LUA_VERSION}...")
            tar_path = f"lua-{LUA_VERSION}.tar.gz"
            if not os.path.exists(tar_path):
                urllib.request.urlretrieve(LUA_URL, tar_path)
            
            print("Extracting Lua...")
            with tarfile.open(tar_path) as tar:
                def is_within_directory(directory, target):
                    abs_directory = os.path.abspath(directory)
                    abs_target = os.path.abspath(target)
                    prefix = os.path.commonprefix([abs_directory, abs_target])
                    return prefix == abs_directory
                
                def safe_extract(tar, path=".", members=None, *, numeric_owner=False):
                    for member in tar.getmembers():
                        member_path = os.path.join(path, member.name)
                        if not is_within_directory(path, member_path):
                            raise Exception("Attempted Path Traversal in Tar File")
                    tar.extractall(path, members, numeric_owner=numeric_owner) 
                    
                safe_extract(tar)

        # Update include_dirs to find lua.h
        self.include_dirs.append(LUA_SRC_DIR)
        
        # Add Lua sources to the extension
        # We need all .c files in src/ EXCEPT lua.c (CLI) and luac.c (Compiler CLI)
        # also onepua.c is usually a concatenation, we should avoid duplicates if present, 
        # but 5.4.7 source usually has individual files. "onelua.c" is often "lauxlib.c" etc combined.
        # Actually standard lua source has all individual files.
        
        lua_sources = glob.glob(os.path.join(LUA_SRC_DIR, "*.c"))
        lua_sources = [
            f for f in lua_sources 
            if "lua.c" not in f and "luac.c" not in f
        ]
        
        # We need to modify the extension headers/sources in place
        for ext in self.extensions:
            if ext.name == "_luaward":
                ext.include_dirs.extend(self.include_dirs)
                ext.sources.extend(lua_sources)
                # Ensure we define LUA_USE_LINUX or similar for proper POSIX features if needed
                ext.define_macros.append(('LUA_USE_LINUX', None))

        super().run()

# Read long description from README.md
with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

setup(
    name="luaward",
    version="1.0.0",
    description="A secure, isolated, and memory-limited Lua 5.4 VM binding for Python.",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/yourusername/luaward",
    author="Your Name",
    author_email="your.email@example.com",
    classifiers=[
        "Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: C",
        "Topic :: Software Development :: Libraries :: Python Modules",
    ],
    packages=["luaward"],
    ext_modules=[Extension('_luaward', ['luaward.c'], 
                           include_dirs=['lua-5.4.7/src'],
                           extra_compile_args=['-DLUA_USE_LINUX'])],
    cmdclass={'build_ext': BuildLuaExt},
    python_requires=">=3.7",
)
