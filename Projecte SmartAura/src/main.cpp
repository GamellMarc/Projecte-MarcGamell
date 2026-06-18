#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <FastLED.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>

// ====================================================================
// 1. CONFIGURACIONS I PINS
// ====================================================================
#define I2C_SDA 8
#define I2C_SCL 9
Adafruit_SSD1306 display(128, 64, &Wire, -1);

#define I2S_SCK 15  
#define I2S_WS  16  
#define I2S_SD  17  
#define I2S_PORT I2S_NUM_0

#define LED_PIN     5       

// OPTIMITZACIÓ: La teva pantalla ara té exactament 113 LEDs físics reals (30 + 53 + 30)
#define NUM_LEDS    113     
#define ACTIVE_LEDS 113     
#define CENTER_LED  56      // El centre matemàtic de dalt de la pantalla

CRGB leds[NUM_LEDS];

#define BUTTON_PIN  4

const uint16_t samples = 1024;
const double samplingFrequency = 16000.0;
double vReal[samples];
double vImag[samples];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, samples, samplingFrequency);

struct AudioData {
  int volume;
  int peakFreq;
};

// ====================================================================
// 2. VARIABLES GLOBALS I WIFI / MQTT / UDP
// ====================================================================
const char* ssid = "Nautilus";
const char* password = "20000Leguas";
const char* mqtt_server = "192.168.50.110"; // IP del teu PC



//const char* ssid = "vodafoneF400";
//const char* password = "Teresa55";
//const char* mqtt_server = "192.168.0.29"; 
const int udp_port = 4210; 

WiFiClient espClient;
PubSubClient client(espClient);
WiFiUDP udp;

volatile int displayVolume = 0;
volatile int displayFreq = 0;
volatile bool systemEnabled = true;

// 0=Mic | 1=Estàtic | 2=Estudi | 3=PC (UDP) | 4=IA Dinàmic
volatile int systemMode = 0; 
volatile uint8_t staticColor = 0; 

// Paràmetres del Mode IA Dinàmic (Mode 4)
volatile int iaFxType = 0;   // 0=Respiració, 1=Ona, 2=Escàner
volatile uint8_t iaColor = 0;  // Hue (0-255)
volatile uint8_t iaSpeed = 50; // Velocitat (1-100)

QueueHandle_t audioQueue;
TaskHandle_t TaskAudioHandle;
TaskHandle_t TaskLEDHandle;
TaskHandle_t TaskHMIHandle;
TaskHandle_t TaskMQTTHandle;
TaskHandle_t TaskUDPHandle; 

// --- GESTIÓ MQTT ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];
  
  message.trim(); message.replace("\"", ""); message.replace("'", ""); message.toUpperCase(); 

  if (message == "PAUSA") systemEnabled = false;
  else if (message == "REPREN") systemEnabled = true;
  else if (message == "MODE_REACTIU") systemMode = 0;
  else if (message == "MODE_ESTUDI") systemMode = 2; 
  else if (message == "MODE_USB") systemMode = 3; 
  else if (message.startsWith("COLOR_ESTATIC:")) {
    systemMode = 1; 
    staticColor = message.substring(message.indexOf(":") + 1).toInt(); 
  }
  else if (message.startsWith("MODE_IA:")) {
    systemMode = 4;
    int firstColon = message.indexOf(':');
    int secondColon = message.indexOf(':', firstColon + 1);
    int thirdColon = message.indexOf(':', secondColon + 1);
    
    if (firstColon != -1 && secondColon != -1 && thirdColon != -1) {
      int fx = message.substring(firstColon + 1, secondColon).toInt();
      int col = message.substring(secondColon + 1, thirdColon).toInt();
      int spd = message.substring(thirdColon + 1).toInt();
      
      iaFxType = constrain(fx, 0, 2);
      iaColor = constrain(col, 0, 255);
      iaSpeed = constrain(spd, 1, 100);
    }
  }
}

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

// ====================================================================
// 3. TASQUES FREERTOS
// ====================================================================

void TaskAudioFFT(void *pvParameters) {
  int32_t *raw_samples = (int32_t *)malloc(samples * sizeof(int32_t));
  size_t bytes_read;
  for (;;) {
    if (systemEnabled && systemMode == 0 && raw_samples != NULL) {
      long sum = 0;
      for(int i = 0; i < samples; i++) {
        i2s_read(I2S_PORT, &raw_samples[i], sizeof(int32_t), &bytes_read, portMAX_DELAY);
        raw_samples[i] >>= 14; sum += raw_samples[i];
      }
      int32_t mean = sum / samples;
      int32_t realVolume = 0;
      for(int i = 0; i < samples; i++) {
        int32_t sample_clean = raw_samples[i] - mean;
        vReal[i] = (double)sample_clean; vImag[i] = 0.0;
        if (abs(sample_clean) > realVolume) realVolume = abs(sample_clean);
      }
      FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
      FFT.compute(FFTDirection::Forward);
      FFT.complexToMagnitude();
      double peakFreq = 0; double maxAmpFFT = 0;
      for(int i = 4; i < (samples/2); i++) { 
        if (vReal[i] > maxAmpFFT) { maxAmpFFT = vReal[i]; peakFreq = ((double)i * samplingFrequency) / samples; }
      }
      AudioData dataToSend = {realVolume, (int)peakFreq};
      xQueueSend(audioQueue, &dataToSend, 0); 
      displayVolume = realVolume; displayFreq = (int)peakFreq;
    } else {
      int32_t dummy; i2s_read(I2S_PORT, &dummy, sizeof(dummy), &bytes_read, 0); 
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void TaskUDP(void *pvParameters) {
  bool udpStarted = false;
  char packetBuffer[255];
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!udpStarted) { udp.begin(udp_port); udpStarted = true; }
      int packetSize = udp.parsePacket();
      if (packetSize) {
        int len = udp.read(packetBuffer, 255);
        if (len > 0) packetBuffer[len] = 0;
        if (systemEnabled && systemMode == 3) {
          String pkt = String(packetBuffer);
          int vIdx = pkt.indexOf("V:"); int fIdx = pkt.indexOf(",F:");
          if (vIdx != -1 && fIdx != -1) {
            int vol = pkt.substring(vIdx+2, fIdx).toInt(); int freq = pkt.substring(fIdx+3).toInt();
            AudioData dataToSend = {vol, freq}; xQueueSend(audioQueue, &dataToSend, 0); 
            displayVolume = vol; displayFreq = freq;
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void TaskLEDs(void *pvParameters) {
  AudioData receivedData;
  for (;;) {
    if (systemEnabled) {
      if (systemMode == 0 || systemMode == 3) { 
        if (xQueueReceive(audioQueue, &receivedData, pdMS_TO_TICKS(30)) == pdTRUE) {
          int maxLedsToLight = (ACTIVE_LEDS / 2) + 1; 
          
          if (receivedData.volume > 200) { 
            int constrainedFreq = constrain(receivedData.peakFreq, 150, 1200);
            uint8_t colorHue = map(constrainedFreq, 150, 1200, 0, 160); 
            int ledsToLight = map(receivedData.volume, 200, 28000, 0, maxLedsToLight);
            ledsToLight = constrain(ledsToLight, 0, maxLedsToLight);

            fadeToBlackBy(leds, NUM_LEDS, 100);
            for(int i = 0; i < ledsToLight; i++) {
              int rightIndex = CENTER_LED + i;       
              int leftIndex  = CENTER_LED - 1 - i;   
              if (rightIndex < ACTIVE_LEDS) leds[rightIndex] = CHSV(colorHue, 255, 255);
              if (leftIndex >= 0)           leds[leftIndex]  = CHSV(colorHue, 255, 255);
            }
          } else { fadeToBlackBy(leds, NUM_LEDS, 100); }
          FastLED.show();
        } else {
          fadeToBlackBy(leds, NUM_LEDS, 20); FastLED.show();
          if (systemMode == 3) displayVolume = 0; 
        }
      } 
      else if (systemMode == 1) {
        xQueueReceive(audioQueue, &receivedData, 0); 
        FastLED.clear();
        fill_solid(leds, ACTIVE_LEDS, CHSV(staticColor, 255, 200)); 
        FastLED.show(); vTaskDelay(pdMS_TO_TICKS(100)); 
      }
      else if (systemMode == 2) {
        xQueueReceive(audioQueue, &receivedData, 0); 
        uint8_t wave = sin8(millis() / 45); uint8_t brightness = map(wave, 0, 255, 0, 220);
        FastLED.clear(); 
        fill_solid(leds, ACTIVE_LEDS, CRGB(brightness, brightness, brightness)); 
        FastLED.show(); vTaskDelay(pdMS_TO_TICKS(10)); 
      }
      else if (systemMode == 4) {
        xQueueReceive(audioQueue, &receivedData, 0); 
        
        if (iaFxType == 0) {
          int divisor = map(iaSpeed, 1, 100, 150, 10);
          uint8_t wave = sin8(millis() / divisor); uint8_t brightness = map(wave, 0, 255, 0, 220);
          FastLED.clear(); fill_solid(leds, ACTIVE_LEDS, CHSV(iaColor, 255, brightness));
        } 
        else if (iaFxType == 1) {
          int divisor = map(iaSpeed, 1, 100, 80, 5);
          uint8_t offset = millis() / divisor; FastLED.clear();
          for(int i = 0; i < ACTIVE_LEDS; i++) {
            uint8_t lowBr = sin8((i * 4) + offset); leds[i] = CHSV(iaColor, 255, map(lowBr, 0, 255, 20, 255));
          }
        } 
        else if (iaFxType == 2) {
          fadeToBlackBy(leds, NUM_LEDS, 35); 
          int bpm = map(iaSpeed, 1, 100, 5, 80); int pos = beatsin16(bpm, 0, ACTIVE_LEDS - 1);
          leds[pos] = CHSV(iaColor, 255, 255);
          if (pos > 0) leds[pos - 1] = CHSV(iaColor, 255, 180);
          if (pos < ACTIVE_LEDS - 1) leds[pos + 1] = CHSV(iaColor, 255, 180);
        }
        FastLED.show(); vTaskDelay(pdMS_TO_TICKS(15));
      }
    } else {
      FastLED.clear(); FastLED.show(); vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// DISSENY PREMIUM DE LA TASCA INTERFÍCIE (OLED UI/UX)
void TaskHMI(void *pvParameters) {
  pinMode(BUTTON_PIN, INPUT_PULLUP); unsigned long lastButtonPress = 0;
  for (;;) {
    if (digitalRead(BUTTON_PIN) == LOW && (millis() - lastButtonPress > 250)) {
      systemEnabled = !systemEnabled; lastButtonPress = millis();
    }
    
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // 1. CAPÇALERA DE DISPÒSITIU FIXA
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("SMART-AURA v1.0");

    // Disseny de l'Icona de Cobertura WiFi amb barres dinàmiques
    if(WiFi.status() == WL_CONNECTED) {
      display.fillRect(114, 6, 2, 2, SSD1306_WHITE);
      display.fillRect(118, 4, 2, 4, SSD1306_WHITE);
      display.fillRect(122, 1, 2, 7, SSD1306_WHITE);
    } else {
      display.drawRect(114, 2, 10, 6, SSD1306_WHITE);
      display.drawFastHLine(116, 5, 6, SSD1306_WHITE);
    }
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE); // Línia separadora

    // 2. RENDERITZAT SEGUONS L'ESTAT DEL DISPOSITIU
    if (systemEnabled) {
      // --- AUDIO & PC AUDIO (MODES 0 i 3) ---
      if (systemMode == 0 || systemMode == 3) {
        display.setCursor(0, 14);
        if (systemMode == 0) display.print("MODE: MIC REACTIU");
        else display.print("MODE: PC AUDIO [UDP]");

        display.setTextSize(2);
        display.setCursor(0, 25);
        display.print(displayFreq); 
        display.setTextSize(1);
        display.print(" Hz");

        // Barra de volum horitzontal emmarcada professional
        display.setCursor(0, 48);
        display.print("VOL");
        display.drawRect(26, 48, 102, 9, SSD1306_WHITE); 
        int barW = map(displayVolume, 200, 28000, 0, 98);
        display.fillRect(28, 50, constrain(barW, 0, 98), 5, SSD1306_WHITE);
      } 
      // --- COLOR FIX (MODE 1) ---
      else if (systemMode == 1) {
        display.setCursor(0, 14); display.print("MODE: AMBIENT FIX");
        
        display.setTextSize(2);
        display.setCursor(0, 26); display.print("HUE: "); display.print(staticColor);

        // Barra indicadora de la posició del Color a la roda cromàtica
        display.setTextSize(1);
        display.drawRect(0, 50, 128, 7, SSD1306_WHITE);
        int hueBar = map(staticColor, 0, 255, 0, 124);
        display.fillRect(2, 52, hueBar, 3, SSD1306_WHITE);
      }
      // --- MODE ESTUDI (MODE 2) ---
      else if (systemMode == 2) {
        display.setCursor(0, 14); display.print("MODE: CONCENTRACIO");
        display.setTextSize(2);
        display.setCursor(18, 26); display.print("FOCUS ACTIVE");
        display.setTextSize(1);
        display.setCursor(5, 50); display.print(">> Batec suau blanc <<");
      }
      // --- MODE COGNITIU INTEL·LIGÈNCIA ARTIFICIAL (MODE 4) ---
      else if (systemMode == 4) {
        display.setCursor(0, 14); display.print("MODE: IA COGNITIVA");
        
        display.setCursor(0, 25); display.print("FX: ");
        if (iaFxType == 0) display.print("RESPIRACIO");
        else if (iaFxType == 1) display.print("ONA FLUIDA");
        else if (iaFxType == 2) display.print("ESCANEJAR");

        display.setCursor(0, 37); display.print("Color triat: "); display.print(iaColor); display.print(" Hue");

        display.setCursor(0, 50); display.print("Vel:");
        display.drawRect(28, 50, 100, 8, SSD1306_WHITE);
        int speedBar = map(iaSpeed, 1, 100, 0, 96);
        display.fillRect(30, 52, speedBar, 4, SSD1306_WHITE);
      }
    } else {
      // INTERFÍCIE INVERTIDA PREMIUM QUAN ESTÀ EN PAUSA
      display.fillRect(14, 20, 100, 30, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(2);
      display.setCursor(26, 28);
      display.print("PAUSA");
    }
    
    display.display();
    vTaskDelay(pdMS_TO_TICKS(40)); 
  }
}

void TaskMQTT(void *pvParameters) {
  client.setServer(mqtt_server, 1883); client.setCallback(mqttCallback);
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        if (client.connect(("NodeA05-"+String(random(0xffff), HEX)).c_str())) {
          client.subscribe("smartaura/control"); 
          Serial.print("\n=== CONNECTAT! LA IP DE L'ESP32 ES: "); Serial.println(WiFi.localIP()); 
        } else vTaskDelay(pdMS_TO_TICKS(5000)); 
      } else client.loop(); 
    }
    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 4500); FastLED.clear(); FastLED.show();
  Wire.begin(I2C_SDA, I2C_SCL); display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  setupI2S(); WiFi.begin(ssid, password);
  
  audioQueue = xQueueCreate(3, sizeof(AudioData));
  if (audioQueue != NULL) {
    xTaskCreatePinnedToCore(TaskAudioFFT, "MicFFT", 8192, NULL, 3, &TaskAudioHandle, 1);
    xTaskCreatePinnedToCore(TaskLEDs, "LEDsAnim", 4096, NULL, 2, &TaskLEDHandle, 1);
    xTaskCreatePinnedToCore(TaskHMI, "OLED", 4096, NULL, 1, &TaskHMIHandle, 0);
    xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 4096, NULL, 1, &TaskMQTTHandle, 0);
    xTaskCreatePinnedToCore(TaskUDP, "UDP_PC", 4096, NULL, 2, &TaskUDPHandle, 0);
  }
}
void loop() { vTaskDelete(NULL); }