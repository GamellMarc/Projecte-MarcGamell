import paho.mqtt.client as mqtt
import google.generativeai as genai
import sys

# ==========================================
# 1. CONFIGURACIÓ
# ==========================================
GEMINI_API_KEY = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
MQTT_BROKER = "127.0.0.1" 

# ==========================================
# 2. INICIALITZACIÓ
# ==========================================
try:
    genai.configure(api_key=GEMINI_API_KEY)
    model = genai.GenerativeModel('gemini-2.5-flash')
except Exception as e:
    print(f"Error configurant la IA: {e}")
    sys.exit()

# Fem servir Localhost perquè Python i Mosquitto estan al mateix PC
MQTT_BROKER = "127.0.0.1" 

try:
    # Solucionem el DeprecationWarning fixant la versió a la 1
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
except AttributeError:
    # Per seguretat si la teva instal·lació és mixta
    client = mqtt.Client()

try:
    client.connect(MQTT_BROKER, 1883, 60)
    client.loop_start() # Iniciem el fil de xarxa en segon pla
except Exception as e:
    print(f"❌ Error connectant al Mosquitto: {e}")
    print("Motius possibles: El CMD del mosquitto no està obert, o el Firewall de Windows l'està bloquejant.")
    sys.exit()
# ==========================================
# 3. PROMPT ENGINEERING (INSTRUCCIONS DE CONTROL)
# ==========================================
system_instruction = """
Ets el 'Director d'Il·luminació' en temps real del Projecte A.05 (Smart-Aura).
Llegeix l'estat d'ànim o petició de l'usuari i escull la millor comanda MQTT de les següents:

ORDRES BÀSIQUES:
- MODE_ESTUDI (Pols blanc estàndard, màxima concentració fix).
- MODE_REACTIU (Vúmetre clàssic per al micròfon, només si demanen festa en directe o parlar amb la veu).
- COLOR_ESTATIC:X (on X és 0-255, per a colors completament fixos i avorrits).

ORDRES AVANÇADES D'AMBIENTACIÓ DINÀMICA (Fes-les servir per a descripcions riques, estats d'ànim o efectes mòbils):
Sintaxi: MODE_IA:EFECTE:COLOR:VELOCITAT
On has de calcular tu cada paràmetre:
1. EFECTE (número del 0 al 2):
   - 0 = Respiració suau/Pulsació (efectes relaxants, íntims, calmats).
   - 1 = Ona fluida viatjant (efectes de fluid, natura, aigua, vent, futuristes, màgics).
   - 2 = Escàner costat a costat (efectes dinàmics, alerta, cinemàtics, robòtics, joc actiu).
2. COLOR (número de la roda cromàtica del 0 al 255):
   - 0=Vermell, 32=Taronja/Foc, 64=Groc, 96=Verd, 130=Cian, 160=Blau, 192=Lila, 224=Rosa.
3. VELOCITAT (número de l'1 al 100):
   - 1 a 20 = Ultra lent i relaxant.
   - 21 a 60 = Normal, orgànic.
   - 61 a 100 = Ràpid, estressant, enèrgic.

REGLA INVIOLABLE: Respon ÚNICAMENT la comanda final en text pla. No afegeixis cap paraula de text humà ni explicació.
"""

print("===================================================")
print("🧠 SMART-AURA AI PARAMETRIC DIRECTOR ACTIVE")
print("===================================================")

while True:
    user_input = input("\n> Quin ambient vols crear? (o 'sortir'): ")
    
    if user_input.lower() == 'sortir':
        break
    if user_input.strip() == "":
        continue

    try:
        prompt_complet = system_instruction + "\nPetició de l'usuari: " + user_input
        response = model.generate_content(prompt_complet)
        comanda = response.text.strip().upper()
        
        print(f"🤖 La IA ha dissenyat la comanda: {comanda}")
        client.publish("smartaura/control", comanda)
        
    except Exception as e:
        print(f"❌ Error: {e}")