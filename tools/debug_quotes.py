FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    for i, line in enumerate(f, 1):
        if 'DockBuilderDockWindow' in line and '.c_str()' in line:
            # Show the raw characters around .c_str()
            pos = line.find('.c_str()')
            if pos >= 0:
                snippet = line[pos:pos+20]
                print(f"Line {i}: {repr(snippet)}")
                for ch in snippet:
                    print(f"  U+{ord(ch):04X} {ch}")
