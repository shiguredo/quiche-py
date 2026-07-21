.PHONY: wheel develop test format lint typecheck

wheel:
	uv build --wheel

develop:
	uv sync --group e2e

test: develop
	uv run pytest tests/

format:
	uv run ruff format src/ tests/

lint:
	uv run ruff check src/ tests/

typecheck:
	uvx ty check
