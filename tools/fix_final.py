FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    c = f.read()

# The pattern is: .c_str()"  (extra quote after close paren)
# We need: .c_str()
c = c.replace('.c_str()",', '.c_str(),')

# Verify it worked
if '.c_str()",' in c:
    print("STILL BROKEN")
else:
    print("Fixed!")
    with open(FP, "w", encoding="utf-8") as f:
        f.write(c)
