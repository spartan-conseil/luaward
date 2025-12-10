PYTHON=python3
PIP=pip
FLAGS=--inplace

.PHONY: all build install test clean examples

all: build

build:
	$(PYTHON) setup.py build_ext $(FLAGS)
	$(PYTHON) setup.py sdist bdist_wheel
	@echo "Build complete."

install:
	$(PIP) install .

test: build
	$(PYTHON) -m unittest discover tests

clean:
	rm -rf build/ dist/ *.egg-info *.so */__pycache__ __pycache__ examples/__pycache__ tests/__pycache__

examples: build
	@echo "Running Basic Usage Example..."
	PYTHONPATH=. $(PYTHON) examples/basic_usage.py
	@echo "\nRunning Callbacks Example..."
	PYTHONPATH=. $(PYTHON) examples/callbacks.py
	@echo "\nRunning Sandboxing Example..."
	PYTHONPATH=. $(PYTHON) examples/sandboxing.py
