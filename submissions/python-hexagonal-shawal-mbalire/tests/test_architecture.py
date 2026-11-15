"""The dependency rule, enforced: the core imports only itself and the standard library.

Every import statement in feed/domain and feed/application is parsed (not executed). Domain may
import only domain; application may import domain and application. Neither may import an adapter,
the runtime, or any third-party package. Breaking the hexagon fails the build.
"""
import ast
import sys
import unittest
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent.parent / "feed"
ALLOWED = {"domain": {"feed.domain"}, "application": {"feed.domain", "feed.application"}}


def imports(path: Path, module: str):
    for node in ast.walk(ast.parse(path.read_text())):
        if isinstance(node, ast.Import):
            yield from (alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            if node.level:  # relative: resolve against this module's package
                base = module.split(".")[: -node.level]
                yield ".".join(base + ([node.module] if node.module else []))
            else:
                yield node.module


class DependencyRule(unittest.TestCase):
    def test_core_depends_only_inward(self):
        for layer, allowed in ALLOWED.items():
            for path in (PACKAGE / layer).rglob("*.py"):
                parts = path.relative_to(PACKAGE.parent).with_suffix("").parts
                module = ".".join(parts[:-1] if parts[-1] == "__init__" else parts)
                if parts[-1] == "__init__":
                    module += ".__init__"  # so level-1 relative imports resolve to the package
                for name in imports(path, module):
                    top = name.split(".")[0]
                    ok = top in sys.stdlib_module_names or any(
                        name == a or name.startswith(a + ".") for a in allowed)
                    self.assertTrue(ok, f"{path.relative_to(PACKAGE.parent)} imports {name}")
