import serial
import pyaudio
import sys
import wave
import time

# --- SETUP VARIABLES ---
PORT = 'COM3'      
BAUD = 921600      
SAMPLE_RATE = 22050
CHUNK_SIZE = 512
CHANNELS = 1
SAMPLE_WIDTH = 2   # 16-bit audio = 2 bytes per sample

filename = f"bird_synth_record_{int(time.time())}.wav"

print(f"Connecting to {PORT} at {BAUD} baud...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"Error opening port: {e}")
    sys.exit()

# Setup Audio Playback
p = pyaudio.PyAudio()
stream = p.open(format=pyaudio.paInt16, channels=CHANNELS, rate=SAMPLE_RATE, output=True)

# Setup WAV File Recording
wf = wave.open(filename, 'wb')
wf.setnchannels(CHANNELS)
wf.setsampwidth(p.get_sample_size(pyaudio.paInt16))
wf.setframerate(SAMPLE_RATE)

print(f"\n🔊 LIVE AUDIO STREAM ACTIVE 🔊")
print(f"🔴 RECORDING TO: {filename}")
print("Press Ctrl+C in this terminal to stop and save the file.\n")

try:
    ser.reset_input_buffer()
    
    while True:
        # Read the text string from the ESP32
        line = ser.readline().decode('ascii', errors='ignore').strip()

        if line.startswith("SYNC:"):
            data = line.replace("SYNC:", "").split(",")
            if len(data) == 4:
                pitch, mod, cad, env = data
                sys.stdout.write(f"\r[ESP32] PIT: {pitch:<4} | MOD: {mod:<4} | CAD: {cad:<4} | ENV: {env:<1}   ")
                sys.stdout.flush()

            # Read the raw audio binary data
            raw_audio = ser.read(CHUNK_SIZE * 2)
            
            if len(raw_audio) == CHUNK_SIZE * 2:
                # 1. Play the audio out loud
                stream.write(raw_audio)
                # 2. Save the audio directly to the WAV file
                wf.writeframes(raw_audio)

except KeyboardInterrupt:
    print("\n\n⏹️ Stopping stream and saving recording...")
finally:
    wf.close()
    stream.stop_stream()
    stream.close()
    p.terminate()
    ser.close()
    print(f"✅ Audio successfully saved to {filename}")