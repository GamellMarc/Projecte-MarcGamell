#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <FastLED.h>
#include <arduinoFFT.h>
#include <WiFi.h>
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
#define NUM_LEDS    300     // Tira completa de 3 metres
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
// 2. VARIABLES GLOBALS I WIFI / MQTT (XARXA LOCAL)
// ====================================================================
const char* ssid = "Nautilus";
const char* password = "20000Leguas";
const char* mqtt_server = "192.168.50.110"; // IP del teu PC
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// Variables d'Estat del Sistema
volatile int displayVolume = 0;
volatile int displayFreq = 0;
volatile bool systemEnabled = true;

// 0 = Reactiu | 1 = Color Estàtic | 2 = Mode Estudi (Respiració Exagerada)
volatile int systemMode = 0; 
volatile uint8_t staticColor = 0; 

QueueHandle_t audioQueue;
TaskHandle_t TaskAudioHandle;
TaskHandle_t TaskLEDHandle;
TaskHandle_t TaskHMIHandle;
TaskHandle_t TaskMQTTHandle;

// --- GESTIÓ DE MISSATGES ENTRANTS MQTT ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  // Neteja blindada de text
  message.trim(); 
  message.replace("\"", "");
  message.replace("'", "");
  message.toUpperCase(); // Passem a majúscules per evitar errors

  Serial.print("Ordre MQTT processada: [");
  Serial.print(message);
  Serial.println("]");

  if (message == "PAUSA") {
    systemEnabled = false;
  } 
  else if (message == "REPREN") {
    systemEnabled = true;
  } 
  else if (message == "MODE_REACTIU") {
    systemMode = 0;
  } 
  else if (message == "MODE_ESTUDI") {
    systemMode = 2; // Activa el mode d'estudi polsant
  }
  else if (message.startsWith("COLOR_ESTATIC:")) {
    systemMode = 1; 
    String colorVal = message.substring(message.indexOf(":") + 1);
    staticColor = colorVal.toInt(); 
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
// 3. IMPLEMENTACIÓ DE TASQUES FREERTOS
// ====================================================================

// --- TASCA 1: ÀUDIO (CORE 1) ---
void TaskAudioFFT(void *pvParameters) {
  int32_t *raw_samples = (int32_t *)malloc(samples * sizeof(int32_t));
  size_t bytes_read;

  for (;;) {
    if (systemEnabled && systemMode == 0 && raw_samples != NULL) {
      long sum = 0;
      for(int i = 0; i < samples; i++) {
        i2s_read(I2S_PORT, &raw_samples[i], sizeof(int32_t), &bytes_read, portMAX_DELAY);
        raw_samples[i] >>= 14; 
        sum += raw_samples[i];
      }

      int32_t mean = sum / samples;
      int32_t realVolume = 0;

      for(int i = 0; i < samples; i++) {
        int32_t sample_clean = raw_samples[i] - mean;
        vReal[i] = (double)sample_clean;
        vImag[i] = 0.0;
        if (abs(sample_clean) > realVolume) realVolume = abs(sample_clean);
      }

      FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
      FFT.compute(FFTDirection::Forward);
      FFT.complexToMagnitude();

      double peakFreq = 0;
      double maxAmpFFT = 0;
      for(int i = 4; i < (samples/2); i++) { 
        if (vReal[i] > maxAmpFFT) {
          maxAmpFFT = vReal[i];
          peakFreq = ((double)i * samplingFrequency) / samples;
        }
      }

      AudioData dataToSend = {realVolume, (int)peakFreq};
      xQueueSend(audioQueue, &dataToSend, 0); 
      displayVolume = realVolume;
      displayFreq = (int)peakFreq;

    } else {
      int32_t dummy;
      i2s_read(I2S_PORT, &dummy, sizeof(dummy), &bytes_read, 0);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// --- TASCA 2: ANIMACIÓ LEDS (CORE 1) ---
void TaskLEDs(void *pvParameters) {
  AudioData receivedData;

  for (;;) {
    if (systemEnabled) {
      if (systemMode == 0) {
        // MODE 0: EQUALITZADOR REACTIU (Center-Out)
        if (xQueueReceive(audioQueue, &receivedData, pdMS_TO_TICKS(30)) == pdTRUE) {
          // Calculem el centre dinàmicament per si de cas, encara que és NUM_LEDS/2
          int centerLED = NUM_LEDS / 2;
          
          if (receivedData.volume > 200) { 
            int constrainedFreq = constrain(receivedData.peakFreq, 150, 1200);
            uint8_t colorHue = map(constrainedFreq, 150, 1200, 0, 160); 
            int ledsToLight = map(receivedData.volume, 200, 28000, 0, centerLED);
            ledsToLight = constrain(ledsToLight, 0, centerLED);

            fadeToBlackBy(leds, NUM_LEDS, 100);

            for(int i = 0; i < ledsToLight; i++) {
              int rightIndex = centerLED + i;       
              int leftIndex  = centerLED - 1 - i;   
              if (rightIndex < NUM_LEDS) leds[rightIndex] = CHSV(colorHue, 255, 255);
              if (leftIndex >= 0)        leds[leftIndex]  = CHSV(colorHue, 255, 255);
            }
          } else {
            fadeToBlackBy(leds, NUM_LEDS, 100);
          }
          FastLED.show();
        }
      } 
      else if (systemMode == 1) {
        // MODE 1: COLOR ESTÀTIC
        xQueueReceive(audioQueue, &receivedData, 0); 
        fill_solid(leds, NUM_LEDS, CHSV(staticColor, 255, 200));
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(100)); 
      }
      else if (systemMode == 2) {
        // MODE 2: MODE ESTUDI (RESPIRACIÓ BLANCA TOTAL EXAGERADA)
        xQueueReceive(audioQueue, &receivedData, 0); // Netegem cua d'àudio
        
        uint32_t ms = millis();
        
        // Generem una ona sinusoïdal suau per a TOTA la tira alhora (perquè respiri)
        // Dividim 'ms' per frenar la velocitat (aprox 4 segons per respiració)
        uint8_t wave = sin8(ms / 20);
        
        // Escalem la brillantor blanca per fer-la EXAGERADA (de 0 a 220)
        // Ara els LEDs s'arribaran a apagar completament (0) i pujaran fins a gairebé el màxim (220)
        uint8_t brightness = map(wave, 0, 255, 0, 220);
        
        // Apliquem la mateixa brillantor blanca a tots els LEDs simultàniament
        fill_solid(leds, NUM_LEDS, CRGB(brightness, brightness, brightness));
        
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(10)); // Actualitzem molt ràpid per suavitat
      }
    } else {
      FastLED.clear();
      FastLED.show();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- TASCA 3: INTERFÍCIE OLED I BOTÓ (CORE 0) ---
void TaskHMI(void *pvParameters) {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  unsigned long lastButtonPress = 0;

  for (;;) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - lastButtonPress > 250) {
        systemEnabled = !systemEnabled;
        lastButtonPress = millis();
      }
    }

    display.clearDisplay();
    
    // Indicador WiFi
    if(WiFi.status() == WL_CONNECTED) display.fillRect(120, 0, 8, 8, SSD1306_WHITE);
    else display.drawRect(120, 0, 8, 8, SSD1306_WHITE);

    if (systemEnabled) {
      if (systemMode == 0) {
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.print(displayFreq); display.println(" Hz");
        display.setTextSize(1);
        display.setCursor(0, 25);
        display.print("Vol: "); display.println(displayVolume);
        int barW = map(displayVolume, 200, 28000, 0, 128);
        barW = constrain(barW, 0, 128);
        display.fillRect(0, 45, barW, 10, SSD1306_WHITE);
      } 
      else if (systemMode == 1) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Projecte A.05");
        display.setCursor(0, 25);
        display.println("MODE NOTIFICACIO");
        display.setCursor(0, 40);
        display.print("Color Hue: "); display.println(staticColor);
      }
      else if (systemMode == 2) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Projecte A.05");
        display.setTextSize(2);
        display.setCursor(0, 22);
        display.println("ESTUDI");
        display.setTextSize(1);
        display.setCursor(0, 48);
        display.println("Pols blanc exagerat"); // Text actualitzat
      }
    } else {
      display.setTextSize(1);
      display.setCursor(10, 25);
      display.println("SISTEMA EN PAUSA");
    }
    
    display.display();
    vTaskDelay(pdMS_TO_TICKS(40)); 
  }
}

// --- TASCA 4: COMUNICACIONS MQTT (CORE 0) ---
void TaskMQTT(void *pvParameters) {
  WiFi.begin(ssid, password);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        String clientId = "ProjecteA05-Node-";
        clientId += String(random(0xffff), HEX);
        
        if (client.connect(clientId.c_str())) {
          client.subscribe("smartaura/control"); 
        } else {
          vTaskDelay(pdMS_TO_TICKS(5000)); 
        }
      } else {
        client.loop(); 
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

// ====================================================================
// 4. SETUP PRINCIPAL
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 4500); 
  FastLED.clear();
  FastLED.show();

  Wire.begin(I2C_SDA, I2C_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;); 
  
  setupI2S();

  audioQueue = xQueueCreate(3, sizeof(AudioData));

  if (audioQueue != NULL) {
    xTaskCreatePinnedToCore(TaskAudioFFT, "AudioFFT", 8192, NULL, 3, &TaskAudioHandle, 1);
    xTaskCreatePinnedToCore(TaskLEDs, "LEDsAnim", 4096, NULL, 2, &TaskLEDHandle, 1);
    xTaskCreatePinnedToCore(TaskHMI, "InterfaceHMI", 4096, NULL, 1, &TaskHMIHandle, 0);
    xTaskCreatePinnedToCore(TaskMQTT, "CommsMQTT", 4096, NULL, 1, &TaskMQTTHandle, 0);
  }
}

void loop() {
  vTaskDelete(NULL);
}