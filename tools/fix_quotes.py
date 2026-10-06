FP = r"C:\Users\SarahRose\PycharmProjects\FrameSync\src\gui\gui_panels.cpp"
with open(FP, "r", encoding="utf-8") as f:
    c = f.read()

# The file has .c_str()"",  and .c_str()"");  patterns
# Fix them by removing the extra " before , or )
c = c.replace('.c_str()"",', '.c_str(),')
c = c.replace('.c_str()"", ', '.c_str(), ')
c = c.replace('.c_str()"");', '.c_str());')

with open(FP, "w", encoding="utf-8") as f:
    f.write(c)
print("done")
