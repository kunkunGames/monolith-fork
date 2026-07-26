import re
with open("Scripts/monolith_proxy.py", "r") as f:
    text = f.read()

if "import re\n" not in text:
    text = text.replace("import os", "import os\nimport re")
text = text.replace('MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")', 'MONOLITH_HEALTH = re.sub(r"/mcp$", "/health", MONOLITH_URL)')

with open("Scripts/monolith_proxy.py", "w") as f:
    f.write(text)
