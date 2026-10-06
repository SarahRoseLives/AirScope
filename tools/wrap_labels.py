import re, os

FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    content = f.read()

# Each tuple: (regex_pattern, replacement_template)
# The regex captures the label text as group 1.
# The replacement wraps it with _L().
patterns = [
    (r'ImGui::TextDisabled\("([^"]+)"\)',            r'ImGui::TextDisabled(_L("\1"))'),
    (r'ImGui::TextColored\(([^,]+), "([^"]+)"\)',    r'ImGui::TextColored(\1, _L("\2"))'),
    (r'ImGui::Combo\("([^#][^"]*)"([,)])',           r'ImGui::Combo(_L("\1")\2'),
    (r'ImGui::Button\("([^#][^"]*)"([,)])',          r'ImGui::Button(_L("\1")\2'),
    (r'ImGui::SmallButton\("([^#][^"]*)"([,)])',     r'ImGui::SmallButton(_L("\1")\2'),
    (r'ImGui::Checkbox\("([^#][^"]*)"([,)])',        r'ImGui::Checkbox(_L("\1")\2'),
    (r'ImGui::SliderFloat\("([^#][^"]*)"([,)])',     r'ImGui::SliderFloat(_L("\1")\2'),
    (r'ImGui::SliderInt\("([^#][^"]*)"([,)])',       r'ImGui::SliderInt(_L("\1")\2'),
    (r'ImGui::InputText\("([^#][^"]*)"([,)])',       r'ImGui::InputText(_L("\1")\2'),
    (r'ImGui::InputTextWithHint\("(##[^"]+)", "([^"]+)"', r'ImGui::InputTextWithHint("\1", _L("\2")'),
    (r'ImGui::RadioButton\("([^#][^"]*)"([,)])',     r'ImGui::RadioButton(_L("\1")\2'),
    (r'ImGui::MenuItem\("([^#][^"]*)"([,)])',        r'ImGui::MenuItem(_L("\1")\2'),
    (r'CollapsingHeader\("([^#][^"]*)"',             r'CollapsingHeader(_L("\1")'),
]

for pattern, replacement in patterns:
    content = re.sub(pattern, replacement, content)

# Clean up double-wrapping
content = content.replace('_L(_L(', '_L(')
# Fix trailing " quotes left over from collapsed strings
content = re.sub(r'_L\("([^"]+)"\)"', r'_L("\1")', content)

with open(FP, "w", encoding="utf-8") as f:
    f.write(content)

print("done")
