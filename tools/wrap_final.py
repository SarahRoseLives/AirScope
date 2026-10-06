import os, re

FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    content = f.read()

# Step 1: Add include
content = content.replace(
    '#include "decode/band_plan.h"',
    '#include "decode/band_plan.h"\n#include "i18n/i18n.h"'
)

# Step 2: Wrap Begin() calls — careful: match the FULL string literal only,
# not partial matches. Use ImGui::Begin("Title" with the closing quote.
panels = [
    ("Control", "Control"),
    ("Decoders", "Decoders"),
    ("SUs", "SUs"),
    ("Messages", "Messages"),
    ("Aircraft", "Aircraft"),
    ("C-Channel", "C-Channel"),
    ("Network", "Network"),
    ("Flight Map", "Flight Map"),
    ("EGC", "EGC"),
    ("MES", "MES"),
    ("LES", "LES"),
    ("Constellation", "Constellation"),
    ("Voice Calls", "Voice Calls"),
    ("LES Freq", "LES Freq"),
    ("About InmarScope", "About InmarScope"),
]

for en, id_ in panels:
    # Match Begin("Title" — the closing quote is part of the match
    old = 'ImGui::Begin("' + en + '"'
    new = 'ImGui::Begin((std::string(_L("' + en + '")) + "###' + id_ + '").c_str()"'
    content = content.replace(old, new)

# Step 3: Wrap DockBuilder calls
dock_panels = [
    ("Control", "Control"),
    ("Decoders", "Decoders"),
    ("SUs", "SUs"),
    ("Messages", "Messages"),
    ("Aircraft", "Aircraft"),
    ("C-Channel", "C-Channel"),
    ("Network", "Network"),
    ("Flight Map", "Flight Map"),
    ("EGC", "EGC"),
    ("MES", "MES"),
    ("LES", "LES"),
    ("Constellation", "Constellation"),
    ("Voice Calls", "Voice Calls"),
    ("LES Freq", "LES Freq"),
    ("Spectrum", "Spectrum"),
    ("Spectrum (B)", "Spectrum (B)"),
    ("Waterfall", "Waterfall"),
    ("Waterfall (B)", "Waterfall (B)"),
]

for en, id_ in dock_panels:
    old = 'DockBuilderDockWindow("' + en + '"'
    new = 'DockBuilderDockWindow((std::string(_L("' + en + '")) + "###' + id_ + '").c_str()"'
    content = content.replace(old, new)

# Fix the extra quote issue from Begin() replacement
# Original: Begin("Title", → becomes Begin((string) + "###Title").c_str()",
# We need to fix: .c_str()",  → .c_str(),  (remove extra " in replacement)
content = content.replace('.c_str()"",', '.c_str(),')
content = content.replace('.c_str()"");', '.c_str());')

# Step 4: Wrap menu items
content = content.replace('ImGui::BeginMenu("View")', 'ImGui::BeginMenu(_L("View"))')
content = content.replace('ImGui::BeginMenu("Help")', 'ImGui::BeginMenu(_L("Help"))')
content = content.replace('MenuItem("Reset Layout")', 'MenuItem(_L("Reset Layout"))')
content = content.replace('if (ImGui::MenuItem("About")', 'if (ImGui::MenuItem(_L("About"))')

# Step 5: Add Languages submenu
content = content.replace(
    'if (ImGui::MenuItem(_L("About")))\n                app.showAbout = true;\n            ImGui::EndMenu();',
    'if (ImGui::BeginMenu(_L("Languages")))\n            {\n                for (int i = 0; i < (int)Lang::KOUNT; ++i)\n                {\n                    Lang l = (Lang)i;\n                    if (ImGui::MenuItem(i18nName(l), nullptr, app.languageIdx == i))\n                    {\n                        app.languageIdx = i;\n                        i18nSet(l);\n                    }\n                }\n                ImGui::EndMenu();\n            }\n            if (ImGui::MenuItem(_L("About")))\n                app.showAbout = true;\n            ImGui::EndMenu();'
)

with open(FP, "w", encoding="utf-8") as f:
    f.write(content)

print("done - Begin, DockBuilder, menu, Languages")
