import google.generativeai as genai

GEMINI_API_KEY = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"

try:
    genai.configure(api_key=GEMINI_API_KEY)
    print("Connectant amb Google...")
    print("Aquests són els models que tens PERMESOS per generar text:\n")
    
    models_trobats = 0
    for m in genai.list_models():
        if 'generateContent' in m.supported_generation_methods:
            print(f" - {m.name}")
            models_trobats += 1
            
    if models_trobats == 0:
        print("\n❌ ALERTA: No tens cap model desbloquejat (Possible bloqueig regional).")
        
except Exception as e:
    print(f"Error de connexió: {e}")