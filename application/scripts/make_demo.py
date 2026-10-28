"""Create synthetic, low-volume audio solely for repeatable application checks."""
from pathlib import Path
import math
import struct
import sys
import wave
root = Path(sys.argv[1])
for folder, title, frequency in [('伤感/雨夜','01 雨夜 Piano 演示.wav',220),('伤感/回忆','02 往事 演示.wav',262),('紧张/追逐','03 暗巷 演示.wav',330),('温暖/重逢','04 日光 演示.wav',392)]:
    path=root/folder/title; path.parent.mkdir(parents=True,exist_ok=True)
    samples=bytearray()
    for i in range(48000*24):
        t=i/48000
        envelope=(0.1+0.9*math.exp(-2.5*(t%2)))*min(t,1)*min(24-t,1)
        value=int(4200*envelope*(math.sin(2*math.pi*frequency*t)+0.3*math.sin(2*math.pi*frequency*2*t)))
        samples.extend(struct.pack('<hh',value,-value))
    with wave.open(str(path),'wb') as audio:
        audio.setnchannels(2); audio.setsampwidth(2); audio.setframerate(48000); audio.writeframes(samples)
print(root)
