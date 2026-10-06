FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    c = f.read()

# Use a list of (old, new) pairs for readability
reps = [
    ("CollapsingHeader(\"CallHunter (auto-scan for voice)", "CollapsingHeader(_L(\"CallHunter (auto-scan for voice)\")"),
    ("CollapsingHeader(\"Database (SQLite log)", "CollapsingHeader(_L(\"Database (SQLite log)\")"),
    ("CollapsingHeader(\"Display\"", "CollapsingHeader(_L(\"Display\")"),
    ("CollapsingHeader(\"Output (message feed)", "CollapsingHeader(_L(\"Output (message feed)\")"),
    ("Combo(\"Decode baud\",", "Combo(_L(\"Decode baud\"),"),
    ("SliderFloat(\"Averaging\",", "SliderFloat(_L(\"Averaging\"),"),
    ("Checkbox(\"Auto-scale dB\",", "Checkbox(_L(\"Auto-scale dB\"),"),
    ("Button(\"Reset view (fit band)\"", "Button(_L(\"Reset view (fit band)\")"),
    ("Button(\"Refresh devices\"", "Button(_L(\"Refresh devices\")"),
    ("Button(\"Browse...\"", "Button(_L(\"Browse...\")"),
    ("Checkbox(\"Band Plan\",", "Checkbox(_L(\"Band Plan\"),"),
    ("Checkbox(\"Band Plan (B)\"", "Checkbox(_L(\"Band Plan (B)\")"),
    ("Checkbox(\"Log messages to database\"", "Checkbox(_L(\"Log messages to database\")"),
    ("SliderInt(\"Keep DB (days)\"", "SliderInt(_L(\"Keep DB (days)\")"),
    ("SliderInt(\"Font size\"", "SliderInt(_L(\"Font size\")"),
    ("Checkbox(\"Enable CallHunter\"", "Checkbox(_L(\"Enable CallHunter\")"),
    ("SliderFloat(\"Threshold (dB above baseline)\"", "SliderFloat(_L(\"Threshold (dB above baseline)\")"),
    ("SliderInt(\"Confirm frames\"", "SliderInt(_L(\"Confirm frames\")"),
    ("SliderInt(\"Lost frames\"", "SliderInt(_L(\"Lost frames\")"),
    ("Button(\"Stop\",", "Button(_L(\"Stop\"),"),
    ("Button(\"Start\",", "Button(_L(\"Start\"),"),
    ("SliderFloat(\"dB min\",", "SliderFloat(_L(\"dB min\"),"),
    ("SliderFloat(\"dB max\",", "SliderFloat(_L(\"dB max\"),"),
    ("Checkbox(\"Show empty\",", "Checkbox(_L(\"Show empty\"),"),
    ("Checkbox(\"Hide encrypted\",", "Checkbox(_L(\"Hide encrypted\"),"),
    ("SmallButton(\"Clear\"", "SmallButton(_L(\"Clear\")"),
    ("Combo(\"Source\",", "Combo(_L(\"Source\"),"),
    ("Checkbox(\"CPU reduce\"", "Checkbox(_L(\"CPU reduce\")"),
    ("Checkbox(\"Mute\"", "Checkbox(_L(\"Mute\")"),
    ("Checkbox(\"DC block\"", "Checkbox(_L(\"DC block\")"),
    ("Checkbox(\"Bias-T\"", "Checkbox(_L(\"Bias-T\")"),
    ("Checkbox(\"Auto gain (AGC)\"", "Checkbox(_L(\"Auto gain (AGC)\")"),
    ("Combo(\"Sample rate (MHz)\"", "Combo(_L(\"Sample rate (MHz)\")"),
    ("Checkbox(\"Loop\",", "Checkbox(_L(\"Loop\"),"),
    ("SmallButton(\"Remove all\"", "SmallButton(_L(\"Remove all\")"),
    ("Checkbox(\"Record voice calls\"", "Checkbox(_L(\"Record voice calls\")"),
    ("Checkbox(\"Follow C-channel voice\"", "Checkbox(_L(\"Follow C-channel voice\")"),
    ("MenuItem(_L(\"Reset Layout\")", "MenuItem(_L(\"Reset Layout\")"),
    ("Checkbox(\"With position only\"", "Checkbox(_L(\"With position only\")"),
]

for old, new in reps:
    c = c.replace(old, new)

# Clean double-wraps
c = c.replace("_L(_L(", "_L(")

with open(FP, "w", encoding="utf-8") as f:
    f.write(c)

print("Done")
