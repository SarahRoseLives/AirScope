import re

FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    content = f.read()

# Fix null character in InputTextWithHint
content = content.replace('InputTextWithHint("\x00",', 'InputTextWithHint("##searchmsg",')
content = content.replace('InputTextWithHint("\x00",', 'InputTextWithHint("##searchsu",')

# Actually, there could be multiple. Let's use exact context:
fixes = [
    ('"\\x00"', '##searchmsg', '_L("Search..."), app.searchBuf'),
    ('"\\x00"', '##searchsu',  '_L("Search..."), app.searchBuf'),
    ('"\\x00"', '##searchegc', '_L("Search..."), app.searchBuf'),
    ('"\\x00"', '##searchles', '_L("Search..."), app.searchBuf'),
]

# Simpler: find all InputTextWithHint with null
for line in content.split('\n'):
    if 'InputTextWithHint(\"\\x00\"' in line or 'InputTextWithHint("\x00"' in line:
        print(line[:80])

print("---")

with open(FP, "w", encoding="utf-8") as f:
    f.write(content)
