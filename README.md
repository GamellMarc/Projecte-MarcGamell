# Projecte A.05: Smart-Aura 💡🤖

Sistema d'il·luminació ambiental IoT cognitiu amb processament d'àudio digital en temps real. Desenvolupat com a projecte per a l'assignatura de Processadors Digitals a la UPC ESEIAAT. 

Aquest sistema va més enllà de les tires LED convencionals: processa l'àudio de l'entorn de forma local mitjançant DSP (Transformada de Fourier) en un ESP32-S3, i compta amb un mode "Cognitiu" on la Intel·ligència Artificial (Google Gemini) dissenya l'ambient de l'habitació interpretant el llenguatge natural de l'usuari.

## 🗂️ Estructura del Repositori

El repositori conté tant el firmware per al microcontrolador (desenvolupat amb PlatformIO) com els scripts de control per a l'ordinador (Python).

### Firmware (PlatformIO)
* **`📁 Projecte SmartAura/`**: Codi definitiu i final del projecte. Implementa FreeRTOS per a la gestió concurrent als dos nuclis de l'ESP32-S3 (Tasques d'Àudio FFT, MQTT, UDP, Interfície OLED i control de FastLED).
* **`📁 Test1Projecte/`**: Codi d'una primera iteració utilitzada per desenvolupar i provar la màquina d'estats i els primers modes de funcionament lògic.
* **`📁 ProvaComponentsProjecte/`**: Scripts de validació i testeig individual a baix nivell dels components físics (Micròfon I2S, Pantalla I2C, LEDs).

### Programari d'Escriptori (Python)
* **`🐍 ai_director.py`**: El "Director d'Il·luminació". Script que recull peticions en llenguatge natural, consulta l'API de Gemini per determinar paràmetres geomètrics i de color, i envia la comanda a l'ESP32 via MQTT (`MODE_IA:FX:COLOR:VELOCITAT`).
* **`🐍 audio_pont.py`**: Script que intercepta l'àudio intern de l'ordinador (Loopback) i envia les dades al microcontrolador a través de paquets UDP per a una reacció de latència zero sense soroll de fons.
* **`🐍 test_api.py`**: Script de suport per validar la connexió i les claus de l'API de Google Cloud abans d'executar el director d'IA.

### Documentació
* **`📄 PrimeraEntrega_DescripcióProjecte.pdf`**: Documentació teòrica, concepte inicial i especificacions del disseny del projecte.

## 🛠️ Maquinari Utilitzat
* **Microcontrolador:** ESP32-S3 (Dual-Core).
* **Entrada d'Àudio:** Micròfon digital omnidireccional INMP441 (Bus I2S).
* **Actuador:** Tira LED WS2812B (Configuració adaptada a geometria de 69 píxels centrats).
* **Interfície HMI:** Pantalla OLED SSD1306 128x64 (Bus I2C) per a monitorització d'estat i cobertura Wi-Fi.

## ⚙️ Requisits i Instal·lació

**Per al Firmware:**
1. Entorn de desenvolupament [PlatformIO](https://platformio.org/).
2. Llibreries necessàries instal·lades al `platformio.ini`: `FastLED`, `arduinoFFT`, `PubSubClient`, `Adafruit GFX`, `Adafruit SSD1306`.

**Per als Scripts de Python:**
1. Servidor MQTT actiu (ex: Mosquitto MQTT Broker) operant al port `1883
