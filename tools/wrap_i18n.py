import os

BASE = r"C:\Users\SarahRose\PycharmProjects\FrameSync"
FP = os.path.join(BASE, "src", "gui", "gui_panels.cpp")

with open(FP, "r", encoding="utf-8") as f:
    content = f.read()

dock_panels = [
    ("Control", "Control"),
    ("Decoders", "Decoders"),
    ("SUs", "SUs"),
    ("Messages", "Messages"),
    ("Aircraft", "Aircraft"),
    ("C-Channel", "C-Channel"),
    ("Network", "Network"),
    ("Flight Map", "FlightMap"),
    ("EGC", "EGC"),
    ("MES", "MES"),
    ("LES", "LES"),
    ("Constellation", "Constellation"),
    ("Voice Calls", "VoiceCalls"),
    ("LES Freq", "LESFreq"),
    ("Spectrum", "Spectrum"),
    ("Spectrum (B)", "SpectrumB"),
    ("Waterfall", "Waterfall"),
    ("Waterfall (B)", "WaterfallB"),
]

for en, id_ in dock_panels:
    old = 'DockBuilderDockWindow("' + en + '"'
    new = 'DockBuilderDockWindow((std::string(_L("' + en + '")) + "###' + id_ + '").c_str()"'
    content = content.replace(old, new)

headers = [
    "Time", "Freq", "ICAO", "Ctry", "Reg", "Flight", "Lat", "Lon", "Alt", "Age", "Msgs",
    "Lock", "Baud", "Eb/N0", "Dir", "AES", "Lbl", "Text", "Bytes", "Priority", "MsgId",
    "Service", "Message", "Sat", "Ch", "Pkt", "Duration", "Play",
    "MES ID", "Action", "LES", "Decoder", "Freq MHz",
]

for h in headers:
    old = 'TableSetupColumn("' + h + '"'
    new = 'TableSetupColumn(_L("' + h + '"))'
    content = content.replace(old, new)

with open(FP, "w", encoding="utf-8") as f:
    f.write(content)

print("done")
