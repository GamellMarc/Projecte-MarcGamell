import soundcard as sc
import numpy as np
import socket
import time
import warnings

# Silenciem els avisos molestos de Windows
warnings.filterwarnings("ignore")

ESP32_IP = "XXX.XXX.XX.XXX"  # <--- RECORDA POSAR LA IP
UDP_PORT = 4210

# Canviem a 48000Hz perquè Windows no es queixi de "discontinuity"
SAMPLE_RATE = 48000 
CHUNK_SIZE = 1024

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
default_speaker = sc.default_speaker()
print(f"Escoltant: {default_speaker.name}")

try:
    mic = sc.get_microphone(id=str(default_speaker.name), include_loopback=True)
except Exception:
    mic = sc.default_microphone()

print(f"Enviant dades a {ESP32_IP}:{UDP_PORT} ...")
print("===================================================")

with mic.recorder(samplerate=SAMPLE_RATE) as recorder:
    while True:
        data = recorder.record(numframes=CHUNK_SIZE)
        
        if len(data.shape) > 1 and data.shape[1] > 0:
            mono_data = data[:, 0] 
        else:
            mono_data = data 

        # Apugem una mica el multiplicador perquè el Loopback de Windows sol ser fluix
        vol = int(np.max(np.abs(mono_data)) * 40000)

        if vol > 200:
            fft_data = np.abs(np.fft.rfft(mono_data))
            freqs = np.fft.rfftfreq(CHUNK_SIZE, 1/SAMPLE_RATE)
            
            valid_idx = np.where((freqs >= 150) & (freqs <= 1200))[0]
            if len(valid_idx) > 0:
                peak_idx = valid_idx[np.argmax(fft_data[valid_idx])]
                peak_freq = int(freqs[peak_idx])
            else:
                peak_freq = 0
        else:
            peak_freq = 0

        msg = f"V:{vol},F:{peak_freq}"
        sock.sendto(msg.encode(), (ESP32_IP, UDP_PORT))
        
        # CHIVATO: Imprimim l'àudio per pantalla
        print(msg)

        time.sleep(0.01)