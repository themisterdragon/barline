"""Embed web/index.html into the program as src/web_page.inc (run after editing the page)."""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
page = open(os.path.join(root, "web", "index.html"), encoding="utf-8").read()
assert ")WEBPAGE\"" not in page
with open(os.path.join(root, "src", "web_page.inc"), "w", encoding="utf-8") as f:
    f.write("// Generated from web/index.html by tools/embed_web.py: edit the page, then run that.\n")
    f.write('static const char *PAGE = R"WEBPAGE(' + page + ')WEBPAGE";\n')
print("embedded", len(page), "bytes")
