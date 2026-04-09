#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// ─── CONFIGURACIÓN OLED Y RGB ────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H  64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

#define RGB_PIN 48
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ─── PINES ───────────────────────────────────────────────────
const int segPins[7] = {19, 21, 36, 35, 38, 20, 37}; // a b c d e f g
const int pinDP = 47;                                // Punto Decimal
const int digPins[4] = {2, 42, 41, 40};              // Dígitos 1-4
const int LED1=15, LED2=16, BUZZER=3;
const int BTN1=4, BTN2=5, BTN3=6, BTN4=7;
const int SW1=14, SW2=13;

// ─── 7-SEGMENTOS (ÁNODO COMÚN: 0 = ENCENDIDO) ────────────────
const uint8_t SEG[10] = {
  0b1000000, 0b1111001, 0b0100100, 0b0110000, 0b0011001,
  0b0010010, 0b0000010, 0b1111000, 0b0000000, 0b0010000
};
const uint8_t SEG_OFF = 0b1111111;

// Variables de multiplexado (Volatile para Interrupciones)
volatile uint8_t dispBuf[4] = {SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF};
volatile bool dispDP[4] = {false, false, false, false}; 
volatile int digActual = 0;
hw_timer_t * timerMultiplex = NULL;

// ─── MÁQUINA DE ESTADOS Y CONFIG ─────────────────────────────
int estado = 0; // 0:Menu, 1:Tiempo, 2:Reps, 3:Resumen, 4:Terapia, 5:Fin
bool oledNuevo = true;
int cfgTiempo = 180, cfgReps = 5; // Default 3min (180s) y 5 reps
int totTiempo=0, totReps=0, repsHechas=0, razonFin=0;
unsigned long tInicio=0, acumPausa=0, tPausa=0;
bool enPausa=false;

// Timers y Debounce
unsigned long tB1=0, tB2=0, tB3=0, tB4=0;
bool antB1=1, antB2=1, antB3=1, antB4=1;
const int DB = 150;
unsigned long tUltimoClicB1 = 0; // Para doble clic

// Salidas temporales
unsigned long tBuzFin=0, tRgbFin=0;
bool avisoActivo=false;
unsigned long tAvisoFlash=0;
bool avisoFlashOn=false;
int lastSlOled = -1;

// ═════════════════════════════════════════════════════════════
//  INTERRUPCIÓN DE HARDWARE: MULTIPLEXADO (CERO PARPADEO)
// ═════════════════════════════════════════════════════════════
void IRAM_ATTR onTimerMux() {
  // 1. Apagar todos los dígitos (Anti-ghosting)
  for(int i=0; i<4; i++) digitalWrite(digPins[i], LOW);
  
  // 2. Avanzar dígito
  digActual = (digActual + 1) % 4;
  
  // 3. Escribir segmentos
  uint8_t p = dispBuf[digActual];
  for (int i=0; i<7; i++) digitalWrite(segPins[i], (p >> i) & 1);
  
  // 4. Punto decimal (Solo si está activo para ese dígito)
  digitalWrite(pinDP, dispDP[digActual] ? LOW : HIGH);
  
  // 5. Encender dígito actual
  digitalWrite(digPins[digActual], HIGH);
}

// ═════════════════════════════════════════════════════════════
//  FUNCIONES AUXILIARES
// ═════════════════════════════════════════════════════════════

void feedback(uint32_t color, int ms) {
  tBuzFin = millis() + ms;
  tRgbFin = millis() + ms;
  ledcWrite(BUZZER, 128); 
  rgb.setPixelColor(0, color); 
  rgb.show();
}

void apagarDisplay() {
  for(int i=0; i<4; i++) { dispBuf[i] = SEG_OFF; dispDP[i] = false; }
}

void actualizarDisplayFisico(int segRestantes, int reps, bool modoTiempo) {
  if (modoTiempo) {
    int m = segRestantes / 60;
    int s = segRestantes % 60;
    dispBuf[0] = SEG_OFF;
    dispBuf[1] = SEG[m % 10]; 
    dispDP[1]  = true; // Activar el punto decimal: M.SS
    dispBuf[2] = SEG[s / 10];
    dispDP[2]  = false;
    dispBuf[3] = SEG[s % 10];
  } else {
    dispDP[1] = false;
    dispBuf[0] = dispBuf[1] = SEG_OFF;
    dispBuf[2] = (reps >= 10) ? SEG[reps/10] : SEG_OFF;
    dispBuf[3] = SEG[reps % 10];
  }
}

void terminarTerapia(int razon) {
  razonFin = razon; estado = 5; oledNuevo = true;
  apagarDisplay();
  feedback(0, 0); // Limpiar RGB
  ledcWrite(BUZZER, 128); tBuzFin = millis() + 2000; // Buzzer 2s
  digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
}

// ═════════════════════════════════════════════════════════════
//  PANTALLAS OLED
// ═════════════════════════════════════════════════════════════
void drawHeader(const char* title) {
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(15, 2); oled.print(title);
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
  for (int i=0; i<7; i++) pinMode(segPins[i], OUTPUT);
  for (int i=0; i<4; i++) pinMode(digPins[i], OUTPUT);
  pinMode(pinDP, OUTPUT); digitalWrite(pinDP, HIGH);
  pinMode(LED1, OUTPUT); pinMode(LED2, OUTPUT); pinMode(BUZZER, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP); pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP); pinMode(BTN4, INPUT_PULLUP);
  pinMode(SW1, INPUT_PULLUP);  pinMode(SW2, INPUT_PULLUP);

  // NUEVO TIMER API v3.0+: 1 tick = 1us. Alarma cada 2.5ms (400Hz total)
  timerMultiplex = timerBegin(1000000); 
  timerAttachInterrupt(timerMultiplex, &onTimerMux);
  timerAlarm(timerMultiplex, 2500, true, 0); 

  ledcAttach(BUZZER, 1000, 8);
  rgb.begin(); rgb.show();
  Wire.begin(8, 9); Wire.setClock(800000);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay(); oled.display();
}

// ═════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL
// ═════════════════════════════════════════════════════════════
void loop() {
  unsigned long ahora = millis();
  bool b1 = digitalRead(BTN1), b2 = digitalRead(BTN2);
  bool b3 = digitalRead(BTN3), b4 = digitalRead(BTN4);

  // BTN4: Reset global (Si no está en el menú principal)
  if (estado != 0 && antB4 && !b4 && (ahora - tB4 > DB)) {
    estado = 0; oledNuevo = true; apagarDisplay(); 
    digitalWrite(LED1,0); digitalWrite(LED2,0); ledcWrite(BUZZER,0);
    tB4 = ahora;
  }
  antB4 = b4;

  switch (estado) {
    case 0: // Selección de modo
      if (oledNuevo) {
        drawHeader("SELECCION MODO");
        oled.setCursor(5, 25); oled.print("B1: ISOTONICO");
        oled.setCursor(5, 40); oled.print("B2: ESTIRAMIENTO");
        oled.display(); oledNuevo = false;
      }
      if (antB1 && !b1 && (ahora-tB1>DB)) { estado = 1; cfgTiempo = 180; oledNuevo = true; tB1=ahora; }
      break;

    case 1: // Configuración Tiempo (1-5 min)
      if (oledNuevo) {
        drawHeader("DURACION SERIE");
        oled.setTextSize(2); oled.setCursor(35, 25);
        oled.print(cfgTiempo/60); oled.print(":"); if(cfgTiempo%60<10) oled.print("0"); oled.print(cfgTiempo%60);
        oled.display(); oledNuevo = false;
      }
      if (antB1 && !b1 && (ahora-tB1>DB)) { 
        if (cfgTiempo > 60) { cfgTiempo -= 10; oledNuevo = true; } 
        else feedback(rgb.Color(255,0,0), 1000); // Límite rojo
        tB1 = ahora; 
      }
      if (antB2 && !b2 && (ahora-tB2>DB)) { 
        if (cfgTiempo < 300) { cfgTiempo += 10; oledNuevo = true; } 
        else feedback(rgb.Color(255,0,0), 1000); 
        tB2 = ahora; 
      }
      if (antB3 && !b3 && (ahora-tB3>DB)) { estado = 2; cfgReps = 5; oledNuevo = true; tB3=ahora; }
      break;

    case 2: // Configuración Repeticiones (3-12)
      if (oledNuevo) {
        drawHeader("REPETICIONES");
        oled.setTextSize(3); oled.setCursor(50, 25); oled.print(cfgReps);
        oled.display(); oledNuevo = false;
      }
      if (antB1 && !b1 && (ahora-tB1>DB)) { 
        if (cfgReps > 3) { cfgReps--; oledNuevo = true; } 
        else feedback(rgb.Color(255,0,0), 1000);
        tB1 = ahora; 
      }
      if (antB2 && !b2 && (ahora-tB2>DB)) { 
        if (cfgReps < 12) { cfgReps++; oledNuevo = true; } 
        else feedback(rgb.Color(255,0,0), 1000);
        tB2 = ahora; 
      }
      if (antB3 && !b3 && (ahora-tB3>DB)) { estado = 3; oledNuevo = true; tB3=ahora; }
      break;

    case 3: // Resumen e Inicio (Doble clic)
      if (oledNuevo) {
        drawHeader("RESUMEN");
        oled.setCursor(5, 20); oled.print("Tiempo: "); oled.print(cfgTiempo/60); oled.print(" min");
        oled.setCursor(5, 32); oled.print("Reps:   "); oled.print(cfgReps);
        oled.setCursor(5, 50); oled.print("2xB1 para iniciar");
        oled.display(); oledNuevo = false;
      }
      if (antB1 && !b1 && (ahora - tB1 > DB)) {
        if (ahora - tUltimoClicB1 < 500) { // DOBLE CLIC EXITOSO
          totTiempo = cfgTiempo; totReps = cfgReps; repsHechas = 0;
          tInicio = ahora; acumPausa = 0; enPausa = false; oledNuevo = true; estado = 4;
        }
        tUltimoClicB1 = ahora; tB1 = ahora;
      }
      break;

    case 4: // TERAPIA EN PROGRESO
    {
      bool sw1 = digitalRead(SW1); // Pausa
      bool sw2 = digitalRead(SW2); // Modo Display
      
      // Manejo de Pausa (SW1)
      if (sw1 == LOW && !enPausa) { enPausa = true; tPausa = ahora; digitalWrite(LED1, HIGH); oledNuevo=true; }
      if (sw1 == HIGH && enPausa) { enPausa = false; acumPausa += (ahora - tPausa); digitalWrite(LED1, LOW); oledNuevo=true; }

      // Manejo de Display e Indicador LED2 (SW2)
      digitalWrite(LED2, sw2 == LOW ? HIGH : LOW);

      unsigned long transcurrido = enPausa ? (tPausa - tInicio - acumPausa) : (ahora - tInicio - acumPausa);
      int sl = totTiempo - (int)(transcurrido / 1000);
      if (sl < 0) sl = 0;

      // Actualizar el 7-Seg constantemente (Conteos)
      actualizarDisplayFisico(sl, repsHechas, sw2 == HIGH);

      // Doble clic BTN1: Incrementar Repetición
      if (antB1 && !b1 && (ahora - tB1 > DB)) {
        if (ahora - tUltimoClicB1 < 500) { 
          repsHechas++; feedback(rgb.Color(0,0,255), 800); oledNuevo = true; 
        }
        tUltimoClicB1 = ahora; tB1 = ahora;
      }

      // Aviso 15 segundos
      if (sl <= 15 && sl > 0 && !enPausa) {
        if (!avisoActivo) { avisoActivo = true; oledNuevo = true; }
        if (ahora - tAvisoFlash > 375) { // 1.33Hz aprox
          avisoFlashOn = !avisoFlashOn; tAvisoFlash = ahora;
          ledcWrite(BUZZER, avisoFlashOn ? 128 : 0);
          rgb.setPixelColor(0, avisoFlashOn ? rgb.Color(255,200,0) : 0); rgb.show();
        }
      }

      // OLED Update
      if (oledNuevo || sl != lastSlOled) {
        lastSlOled = sl;
        drawHeader(enPausa ? "PAUSADO" : "EN PROGRESO");
        oled.setCursor(5, 18); oled.print("Tiempo: "); oled.print(sl/60); oled.print(":"); if(sl%60<10) oled.print("0"); oled.print(sl%60);
        oled.setCursor(5, 30); oled.print("Reps:   "); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
        if (sl <= 15 && sl > 0) { oled.setCursor(5, 45); oled.print("! FALTAN 15 SEG !"); }
        oled.display(); oledNuevo = false;
      }

      // Condiciones de Fin
      if (repsHechas >= totReps) terminarTerapia(1); // Completado
      else if (sl <= 0)           terminarTerapia(2); // Tiempo agotado
      else if (antB3 && !b3)      terminarTerapia(3); // Interrumpido
    }
    break;

    case 5: // FIN DE SESIÓN
      if (oledNuevo) {
        drawHeader("RESUMEN FINAL");
        oled.setCursor(5, 20); oled.print(razonFin==1?"LOGRADO!":razonFin==2?"TIEMPO AGOTADO":"INTERRUMPIDO");
        oled.setCursor(5, 35); oled.print("Reps: "); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
        oled.setCursor(5, 52); oled.print("B4: SALIR");
        oled.display(); oledNuevo = false;
        avisoActivo = false;
      }
      break;
  }

  // Gestión de timers de salida (Buzzer/RGB)
  if (!avisoActivo) {
    if (ahora >= tBuzFin) ledcWrite(BUZZER, 0);
    if (ahora >= tRgbFin) { rgb.setPixelColor(0, 0); rgb.show(); }
  }
  
  antB1=b1; antB2=b2; antB3=b3;
}
