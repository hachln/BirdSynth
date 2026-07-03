#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- OLED SETTINGS ---
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- ESP32 POTENTIOMETER PINS (ADC1) ---
const int POT_PITCH   = 32;   
const int POT_MOD     = 33;     
const int POT_CADENCE = 34; 
const int POT_ENV     = 35; 

// --- INTERNAL AUDIO SETTINGS ---
const int sampleRate = 22050; 

// --- SHARED VARIABLES (Cross-Core via FreeRTOS) ---
volatile int pitchVal = 0, modVal = 0, cadVal = 0, envVal = 0;
volatile float carrierFreq = 1000.0, modFreq = 10.0, modDepth = 500.0;
volatile int delayBetweenChirps = 500;
volatile int envMode = 0;
unsigned long lastDisplayTime = 0;

// --- AUDIO BUFFERING SYSTEM ---
const int CHUNK_SIZE = 512;
int16_t audioBuffer[CHUNK_SIZE];
volatile int bufferIndex = 0;
volatile bool bufferReady = false;

// Task Handle for Core 0
TaskHandle_t AudioTaskHandle;

// ==========================================
// CORE 1: UI, MATH, & SERIAL COMMS
// ==========================================
void setup() {
  Serial.begin(921600); 

  // Boot the OLED
  Wire.begin();
  Wire.setClock(400000); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Screen failed to boot!");
    for(;;); 
  }
  display.setTextColor(SSD1306_WHITE);

  // Launch the Audio Synthesizer Task on CORE 0
  xTaskCreatePinnedToCore(
      audioSynthTask,    
      "AudioSynth",      
      10000,             
      NULL,              
      1,                 
      &AudioTaskHandle,  
      0                  
  );
}

void loop() {
  // 1. Always read the knobs so audio reacts instantly
  pitchVal = analogRead(POT_PITCH);
  modVal = analogRead(POT_MOD);
  cadVal = analogRead(POT_CADENCE);
  envVal = analogRead(POT_ENV); 

  carrierFreq = map(pitchVal, 0, 4095, 300, 4000); 
  modFreq = map(modVal, 0, 4095, 1, 100);          
  modDepth = carrierFreq * 0.4;                    
  delayBetweenChirps = map(cadVal, 0, 4095, 100, 3000); 
  envMode = constrain(map(envVal, 0, 4095, 0, 3), 0, 3);

  // 2. ONLY draw to the screen every 16ms (approx 60 FPS)
  if (millis() - lastDisplayTime >= 16) {
      display.clearDisplay();
      display.setTextSize(1); 
      
      int pitchBar = map(pitchVal, 0, 4095, 0, 45);
      int modBar   = map(modVal,   0, 4095, 0, 45);
      int cadBar   = map(cadVal,   0, 4095, 0, 45);
      int envBar   = map(envVal,   0, 4095, 0, 45);

      display.setCursor(0, 0); display.print("PIT");
      display.drawRect(22, 0, 45, 10, SSD1306_WHITE);
      display.fillRect(22, 0, pitchBar, 10, SSD1306_WHITE);
      display.setCursor(72, 1); display.print((int)carrierFreq); display.print("Hz");

      display.setCursor(0, 16); display.print("MOD");
      display.drawRect(22, 16, 45, 10, SSD1306_WHITE);
      display.fillRect(22, 16, modBar, 10, SSD1306_WHITE);
      display.setCursor(72, 17); display.print((int)modFreq); display.print("Hz");

      display.setCursor(0, 32); display.print("CAD");
      display.drawRect(22, 32, 45, 10, SSD1306_WHITE);
      display.fillRect(22, 32, cadBar, 10, SSD1306_WHITE);
      display.setCursor(72, 33); display.print(delayBetweenChirps); display.print("ms");

      display.setCursor(0, 48); display.print("ENV");
      display.drawRect(22, 48, 45, 10, SSD1306_WHITE);
      display.fillRect(22, 48, envBar, 10, SSD1306_WHITE);
      
      display.setCursor(72, 49); 
      if (envMode == 0) display.print("SMOOTH");
      else if (envMode == 1) display.print("STACCATO");
      else if (envMode == 2) display.print("GLISSAND"); 
      else if (envMode == 3) display.print("COMPLEX");
      
      display.display();
      
      lastDisplayTime = millis(); // Reset the timer
  }

  // 3. Audio/Python Sync
  if (bufferReady) {
     Serial.print("SYNC:");
     Serial.print(pitchVal); Serial.print(",");
     Serial.print(modVal); Serial.print(",");
     Serial.print(cadVal); Serial.print(",");
     Serial.println(envVal); 

     Serial.write((uint8_t*)audioBuffer, CHUNK_SIZE * 2);
     bufferReady = false;
  }
  
  
}

// ==========================================
// CORE 0: DEDICATED AUDIO GENERATOR 
// ==========================================
float phase_c = 0; 
float phase_m = 0; 

uint32_t stateSampleCount = 0; 
int sequenceState = 0; 
uint32_t currentDurationSamples = 0;

int16_t generateBirdSample(float startFreq, float endFreq, float modF, float modD, float attackMs, float releaseMs, uint32_t totalSamples) {
    float progress = (float)stateSampleCount / totalSamples;
    float current_fc = startFreq + ((endFreq - startFreq) * progress);
    
    float inc_m = (TWO_PI * modF) / sampleRate;
    float mod_wave = sin(phase_m) * modD;
    phase_m += inc_m;
    if (phase_m > TWO_PI) phase_m -= TWO_PI;
    
    current_fc += mod_wave; 
    
    float inc_c = (TWO_PI * current_fc) / sampleRate;
    float out = sin(phase_c);
    phase_c += inc_c;
    if (phase_c > TWO_PI) phase_c -= TWO_PI;
    
    float env = 1.0;
    uint32_t attackSamples = (attackMs / 1000.0) * sampleRate;
    uint32_t releaseSamples = (releaseMs / 1000.0) * sampleRate;
    
    if (stateSampleCount < attackSamples) {
        env = (float)stateSampleCount / attackSamples; 
    } else if (stateSampleCount > (totalSamples - releaseSamples)) {
        env = (float)(totalSamples - stateSampleCount) / releaseSamples; 
    }
    
    return (int16_t)(out * env * 8000.0); 
}

void audioSynthTask(void * pvParameters) {
  // Pacing calculation to ensure exact 22050 Hz speed without hardware limits
  unsigned long nextSampleTime = micros();
  const unsigned long sampleInterval = 1000000 / sampleRate; 
  
  for(;;) { 
    if (!bufferReady) {
        
        // Block until it is time to generate the next sample
        while(micros() < nextSampleTime) { 
           // Busy-wait for microsecond precision 
        }
        nextSampleTime += sampleInterval;

        int16_t sample = 0;
        
        if (sequenceState == 0) {
            currentDurationSamples = (delayBetweenChirps / 1000.0) * sampleRate;
            sample = 0; 
        } else {
            switch (envMode) {
                case 0: 
                    if (sequenceState == 1) {
                        currentDurationSamples = 0.8 * sampleRate; 
                        sample = generateBirdSample(carrierFreq, carrierFreq, modFreq, modDepth, 300, 300, currentDurationSamples);
                    } else { 
                        sequenceState = 0; 
                        stateSampleCount = 0; 
                    }
                    break;
                case 1: 
                    if (sequenceState == 1) {
                        currentDurationSamples = 0.3 * sampleRate; 
                        sample = generateBirdSample(carrierFreq, carrierFreq, modFreq, modDepth, 10, 100, currentDurationSamples);
                    } else { 
                        sequenceState = 0; 
                        stateSampleCount = 0; 
                    }
                    break;
                case 2: 
                    if (sequenceState == 1) {
                        currentDurationSamples = 1.5 * sampleRate; 
                        sample = generateBirdSample(carrierFreq, carrierFreq * 0.4, modFreq, modDepth, 50, 600, currentDurationSamples);
                    } else { 
                        sequenceState = 0; 
                        stateSampleCount = 0; 
                    }
                    break;
                case 3: 
                    if (sequenceState == 1) {
                        currentDurationSamples = 0.8 * sampleRate; 
                        sample = generateBirdSample(carrierFreq, carrierFreq, 0, 0, 150, 150, currentDurationSamples);
                    } else if (sequenceState == 2) {
                        currentDurationSamples = 0.1 * sampleRate; 
                        sample = 0; 
                    } else if (sequenceState == 3) {
                        currentDurationSamples = 0.15 * sampleRate; 
                        sample = generateBirdSample(carrierFreq - 500, carrierFreq + 1000, modFreq, modDepth, 20, 20, currentDurationSamples);
                    } else if (sequenceState == 4) {
                        currentDurationSamples = 0.2 * sampleRate; 
                        sample = generateBirdSample(carrierFreq + 1000, carrierFreq - 1000, modFreq, modDepth, 20, 50, currentDurationSamples);
                    } else { 
                        sequenceState = 0; 
                        stateSampleCount = 0; 
                    }
                    break;
            }
        }
        
        stateSampleCount++;
        
        if (stateSampleCount >= currentDurationSamples) {
            stateSampleCount = 0;
            phase_c = 0; 
            phase_m = 0;
            if (sequenceState == 0) sequenceState = 1; 
            else sequenceState++; 
        }

        audioBuffer[bufferIndex++] = sample;
        
        if (bufferIndex >= CHUNK_SIZE) {
           bufferIndex = 0;
           bufferReady = true;
        }
    } else {
        vTaskDelay(1); 
    }
  }
}