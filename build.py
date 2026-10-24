# Builds dist/RE4_Net_Worth_v<version>.zip from build/re4_net_worth.dll (run
# build.bat first), laid out to extract into the game folder. Zip entries use
# forward slashes.
import os, re, zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(ROOT, "src", "window.cpp"), encoding="utf-8").read()
version = re.search(r'VERSION = "([^"]+)"', src).group(1)
FILES = [
    ("build/re4_net_worth.dll", "reframework/plugins/re4_net_worth.dll"),
    ("reframework/data/re4_net_worth/NetWorth.html", None),
    ("reframework/fonts/re4_net_worth/CourierPrime-Bold.ttf", None),
    ("reframework/fonts/re4_net_worth/OFL.txt", None),
]
os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
out = os.path.join(ROOT, "dist", f"RE4_Net_Worth_v{version}.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for rel, arc in FILES:
        z.write(os.path.join(ROOT, rel), arc or rel)
print(out)
