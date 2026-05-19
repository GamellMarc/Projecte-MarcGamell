#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <FastLED.h>
#include <arduinoFFT.h>

// --- PINS I CONFIGURACIONS ---
#define I2C_SDA 8
#define I2C_SCL 9
Adafruit_SSD1306 display(128, 64, &Wire, -1);

#define I2S_SCK 15  
#define I2S_WS  16  
#define I2S_SD  17  
#define I2S_PORT I2S_NUM_0

#define LED_PIN     5       
#define NUM_LEDS    300     
#define MAX_VU_LEDS 60      
CRGB leds[NUM_LEDS];

// --- CONFIGURACIÓ DEL BOTÓ ---
#define BUTTON_PIN  4
bool systemEnabled = true;       
unsigned long lastButtonPress = 0;

// --- CONFIGURACIÓ FFT ---
const uint16_t samples = 1024;
const double samplingFrequency = 16000.0;
double vReal[samples];
double vImag[samples];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, samples, samplingFrequency);

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

void setup() {
  Serial.begin(115200);
  
  // Configurem el pin del botó
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Inicialitzem LEDs
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 800); 
  FastLED.clear();
  FastLED.show();

  // Inicialitzem OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("ERROR OLED"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("Sistema FFT Llest!");
  display.display();
  delay(1500);
  
  setupI2S();
}

void loop() {
  // --- 1. BOTÓ PAUSA/REPRÈN ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - lastButtonPress > 250) {
      systemEnabled = !systemEnabled;
      lastButtonPress = millis();
      if (!systemEnabled) {
        FastLED.clear();
        FastLED.show();
        display.clearDisplay();
        display.setCursor(10, 20);
        display.println("Sistema Apagat (Zzz)");
        display.display();
      }
    }
  }

  // Si està apagat, netegem el buffer i tornem a dalt
  if (!systemEnabled) {
    int32_t dummy;
    size_t bytes_read;
    i2s_read(I2S_PORT, &dummy, sizeof(dummy), &bytes_read, 0); 
    delay(10);
    return;
  }

  // --- 2. LLEGIR AUDIO I CALCULAR VOLUM REAL ---
  int32_t raw_samples[samples];
  size_t bytes_read;
  long sum = 0;

  // Llegim els 1024 valors d'àudio
  for(int i = 0; i < samples; i++) {
    i2s_read(I2S_PORT, &raw_samples[i], sizeof(int32_t), &bytes_read, portMAX_DELAY);
    raw_samples[i] >>= 14; 
    sum += raw_samples[i];
  }

  int32_t mean = sum / samples;
  int32_t realVolume = 0; // Variable per guardar el volum real (Domini del Temps)

  for(int i = 0; i < samples; i++) {
    // Netegem l'offset (el soroll de fons constant)
    int32_t sample_clean = raw_samples[i] - mean;
    
    // Omplim els arrays per a la FFT
    vReal[i] = (double)sample_clean;
    vImag[i] = 0.0;
    
    // Busquem el pic màxim de volum d'aquest paquet d'àudio
    if (abs(sample_clean) > realVolume) {
      realVolume = abs(sample_clean);
    }
  }

  // --- 3. CÀLCUL MATEMÀTIC (FFT) NOMÉS PEL COLOR ---
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  double peakFreq = 0;
  double maxAmpFFT = 0;
  
  // Analitzem a partir del "bin" 4 per ignorar subgreus fantasma
  for(int i = 4; i < (samples/2); i++) { 
    if (vReal[i] > maxAmpFFT) {
      maxAmpFFT = vReal[i];
      peakFreq = ((double)i * samplingFrequency) / samples;
    }
  }

  // --- 4. ACTUALITZAR OLED I LEDS ---
  // Ara fem servir el 'realVolume' per al llindar de soroll
  if (realVolume > 200) { 
    
    // ---- PANTALLA OLED ----
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print((int)peakFreq); display.println(" Hz");
    
    display.setTextSize(1);
    display.setCursor(0, 25);
    display.print("Volum: "); display.println(realVolume);

    // Barra OLED mapejada al volum real
    int barW = map(realVolume, 200, 10000, 0, 128);
    barW = constrain(barW, 0, 128);
    display.fillRect(0, 40, barW, 10, SSD1306_WHITE);
    display.display();

    // ---- TIRA LED ----
    // 1. Color = Freqüència dominant obtinguda de la FFT
    int constrainedFreq = constrain((int)peakFreq, 150, 1200);
    uint8_t colorHue = map(constrainedFreq, 150, 1200, 0, 160); // 0=Vermell, 160=Blau

    // 2. Longitud = Volum Real obtingut en el pas 2
    int ledsToLight = map(realVolume, 200, 10000, 0, MAX_VU_LEDS);
    ledsToLight = constrain(ledsToLight, 0, MAX_VU_LEDS);

    // 3. Efecte de caiguda visual (trail) perquè no sigui tan abrupte
    fadeToBlackBy(leds, NUM_LEDS, 100);

    // 4. Pintem els LEDs que toquen
    for(int i = 0; i < ledsToLight; i++) {
      leds[i] = CHSV(colorHue, 255, 255);
    }
    FastLED.show();
    
  } else {
    // Silenci...
    fadeToBlackBy(leds, NUM_LEDS, 100); // Apagat progressiu
    FastLED.show();
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("Escoltant ambient...");
    display.display();
  }
}