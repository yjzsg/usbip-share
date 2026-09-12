"""Extract the inline <script> from index.html into tests/inline.js.

test_ui_boot.js runs that file directly, so it always exercises the script the
page really ships. Re-run this whenever index.html changes.
"""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
blocks = re.findall(r"<script>(.*?)</script>", (root / "index.html").read_text(encoding="utf-8"), re.S)
(root / "tests" / "inline.js").write_text("\n".join(blocks), encoding="utf-8")
print(f"extracted {len(blocks)} script block(s) -> tests/inline.js")
