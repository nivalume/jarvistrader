set shell := ["zsh", "-cu"]

python := ".venv/bin/python"

bootstrap:
    test -x {{python}} || uv venv --python 3.11 .venv
    uv pip install --python {{python}} "scikit-build-core==1.0.3" "pytest==9.1.1" "pre-commit==4.6.2"

configure preset="dev": bootstrap
    test -x {{python}}
    cmake --preset {{preset}} -DPython_EXECUTABLE="${PWD}/{{python}}"

build preset="dev": (configure preset)
    cmake --build --preset {{preset}}

install: bootstrap
    uv pip install --python {{python}} --reinstall .

test: (build "dev") install
    ctest --preset dev
    {{python}} -m pytest

fp: (build "rel") (build "det-o0")
    @echo "Fingerprint comparison is deferred until the first kernel state is implemented."

bench: (build "bench")
    cmake --build --preset bench --target jarvis_bench
    @echo "Benchmark JSON is deferred until the first benchmark is implemented."

format:
    rg --files -g '*.cpp' -g '*.hpp' -g '*.h' | xargs bash tools/run-clang-format.sh -i

lint: (configure "dev")
    {{python}} -m pre_commit run --all-files
