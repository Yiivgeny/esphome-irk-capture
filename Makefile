PYTHON_VERSION ?= 3.12
EXAMPLE ?= examples/irk_capture.yaml

.PHONY: sync compile lint

sync:
	uv sync --python $(PYTHON_VERSION)

compile:
	uv run --python $(PYTHON_VERSION) esphome compile $(EXAMPLE)

lint:
	uv run --python $(PYTHON_VERSION) ruff check .
