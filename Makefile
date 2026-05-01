PYTHON_VERSION ?= 3.11.13
EXAMPLE ?= examples/irk_extractor.yaml

.PHONY: sync compile lint

sync:
	uv sync --python $(PYTHON_VERSION)

compile:
	uv run --python $(PYTHON_VERSION) esphome compile $(EXAMPLE)

lint:
	uv run --python $(PYTHON_VERSION) ruff check .
