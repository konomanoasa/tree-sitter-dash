import sys
from pathlib import Path

from setuptools import Extension, setup

Path("dist/metadata").mkdir(parents=True, exist_ok=True)

setup(
    options={
        "build": {"build_base": "dist/build"},
        "egg_info": {"egg_base": "dist/metadata"},
    },
    ext_modules=[
        Extension(
            "tree_sitter_dash.binding",
            sources=[
                "bindings/python/binding.c",
                "src/parser.c",
                "src/scanner.c",
            ],
            include_dirs=["src"],
            depends=[
                "src/arithmetic.h",
                "src/input.h",
                "src/source.h",
                "src/tokens.h",
                "src/tree_sitter/alloc.h",
                "src/tree_sitter/parser.h",
            ],
            extra_compile_args=["/std:c17", "/utf-8"]
            if sys.platform == "win32"
            else ["-std=c17"],
        )
    ],
)
