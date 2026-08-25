// ======================================================================
// SEMAFOROS ESP32 — 2 Semáforos independientes (Carro + Peatón)
// AHORA COORDINADOS ENTRE SÍ + LECTURA DE CONTADOR (SÓLO SEMÁFORO 1)
// + INTEGRACIÓN BLYNK (SIN PWM)
// ======================================================================

#define BLYNK_TEMPLATE_ID "TMPL2AIQN_Nb-"
#define BLYNK_TEMPLATE_NAME "Semáforos Inteligentes"
#define BLYNK_AUTH_TOKEN "I1SmUAszDFEL_dFk42zuk-RzQep3QX8Z"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>

// ====== WiFi (cámbialo por tu red) ======
char ssid[] = "JHON";
char pass[] = "10012418";

// ====== Virtual Pins Blynk ======
#define VP_CAR_COUNT_S1   V0
#define VP_STATE_S1       V1
#define VP_STATE_S2       V2
#define VP_PED_TIME       V3
#define VP_RESET_CNT1     V4
#define VP_RESET_CNT2     V5
#define VP_PED_BTN1       V6
#define VP_PED_BTN2       V7

BlynkTimer timer;

// ================= SEMÁFORO 1 =================
const int redPin1     = 14;
const int yellowPin1  = 27;
const int greenPin1   = 26;
const int pedPin1     = 25;

const int buttonPin1  = 4;  
const int carPin1     = 5;  

// ================= SEMÁFORO 2 =================
const int redPin2     = 13;
const int yellowPin2  = 12;
const int greenPin2   = 23;
const int pedPin2     = 22;

const int buttonPin2  = 21;
const int carPin2     = 19;

// ================= CONTADOR SEMÁFORO 1 (ENTRADAS DESDE FLIP-FLOPS) =================
// Semáforo 1 (contador 1) – QA, QB, QC, QD
const int cnt1_Q0 = 32;  // QA1 = bit 0 (LSB)
const int cnt1_Q1 = 33;  // QB1 = bit 1
const int cnt1_Q2 = 34;  // QC1 = bit 2 (solo entrada)
const int cnt1_Q3 = 35;  // QD1 = bit 3 (MSB)

// ================= RESET CONTADORES (SALIDAS HACIA FLIP-FLOPS) =================
const int resetCnt1Pin = 18; // reset contador S1 (conectado al CLR/RESET del contador 1)
const int resetCnt2Pin = 15; // reset contador S2 (conectado al CLR/RESET del contador 2)

// Tiempos (ms)
const unsigned long RED_TIME    = 4000;
const unsigned long GREEN_TIME  = 4000;
const unsigned long YELLOW_TIME = 2000;
const unsigned long PED_TIME    = 5000;

// Estados
enum State {RED, YELLOW_AFTER_RED, GREEN, YELLOW_AFTER_GREEN, PEDESTRIAN};

// Estado y tiempos de cada semáforo
State state1 = RED;
State state2 = RED;
unsigned long lastChange1 = 0;
unsigned long lastChange2 = 0;

// Flags semáforo 1
bool solicitudPeaton1 = false;
bool pulsoCarro1 = false;
bool vengoDePeaton1 = false;

// Flags semáforo 2
bool solicitudPeaton2 = false;
bool pulsoCarro2 = false;
bool vengoDePeaton2 = false;

// Debounce y estados botón semáforo 1
int lastButtonState1 = HIGH;
unsigned long lastButtonBounce1 = 0;
bool buttonHeld1 = false;

// Debounce y estados botón semáforo 2
int lastButtonState2 = HIGH;
unsigned long lastButtonBounce2 = 0;
bool buttonHeld2 = false;

unsigned long lastButtonPressMillis1 = 0;
unsigned long lastButtonPressMillis2 = 0;

const unsigned long DEBOUNCE_MS = 50;

// ---------- quién está en fase peatonal (0 = nadie, 1 ó 2) ----------
int pedOwner = 0;

// ---------- último valor leído del contador de S1 ----------
int lastCount1 = -1;

// prototipos
void fsm(int id);
void changeState(int id, State newState);
void allOff(int id);
int  readCounter1();   // sólo semáforo 1
String stateToText(State s);
void reportStateToBlynk(int id, State s);
void resetCounter1Pulse();
void resetCounter2Pulse();

// ======================================================================
// SETUP
// ======================================================================
void setup() {
  Serial.begin(115200);

  // ====== Blynk / WiFi ======
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Pines semáforo 1
  pinMode(redPin1, OUTPUT);
  pinMode(yellowPin1, OUTPUT);
  pinMode(greenPin1, OUTPUT);
  pinMode(pedPin1, OUTPUT);
  pinMode(buttonPin1, INPUT_PULLUP);
  pinMode(carPin1, INPUT_PULLUP);

  // Pines semáforo 2
  pinMode(redPin2, OUTPUT);
  pinMode(yellowPin2, OUTPUT);
  pinMode(greenPin2, OUTPUT);
  pinMode(pedPin2, OUTPUT);
  pinMode(buttonPin2, INPUT_PULLUP);
  pinMode(carPin2, INPUT_PULLUP);

  // Pines contador semáforo 1 (entradas desde flip-flops)
  pinMode(cnt1_Q0, INPUT);
  pinMode(cnt1_Q1, INPUT);
  pinMode(cnt1_Q2, INPUT);
  pinMode(cnt1_Q3, INPUT);

  // Pines reset contadores (salidas)
  pinMode(resetCnt1Pin, OUTPUT);
  pinMode(resetCnt2Pin, OUTPUT);
  // Suponiendo reset activo en HIGH:
  digitalWrite(resetCnt1Pin, LOW);
  digitalWrite(resetCnt2Pin, LOW);

  allOff(1);
  allOff(2);

  // Ambos arrancan en rojo (se encargarán de coordinarse en YELLOW/VERDE)
  changeState(1, RED);
  changeState(2, RED);

  // Timer para enviar periódicamente info a Blynk (cada 200 ms)
  timer.setInterval(200L, []() {
    // Tiempo restante de modo peatonal (si alguno está en PEDESTRIAN)
    long remainingPedMs = 0;
    if (state1 == PEDESTRIAN) {
      remainingPedMs = (long)PED_TIME - (long)(millis() - lastChange1);
    } else if (state2 == PEDESTRIAN) {
      remainingPedMs = (long)PED_TIME - (long)(millis() - lastChange2);
    }
    if (remainingPedMs < 0) remainingPedMs = 0;
    float remainingSec = remainingPedMs / 1000.0;
    Blynk.virtualWrite(VP_PED_TIME, remainingSec);
  });
}

// ======================================================================
// LOOP
// ======================================================================
void loop() {
  Blynk.run();
  timer.run();

  fsm(1);
  fsm(2);

  // ====== LECTURA DE CONTADOR 0–15 DESDE LOS FLIP-FLOPS DEL SEMÁFORO 1 ======
  int count1 = readCounter1();

  if (count1 != lastCount1) {
    lastCount1 = count1;
    Serial.printf("CONTADOR S1 (carros) = %d\n", count1);
    Blynk.virtualWrite(VP_CAR_COUNT_S1, count1);
  }

  delay(5);
}

// ==================== FSM ====================
void fsm(int id) {
  unsigned long now = millis();

  // Selección de variables según ID
  volatile State &state = (id == 1 ? state1 : state2);
  volatile bool &solicitudPeaton = (id == 1 ? solicitudPeaton1 : solicitudPeaton2);
  volatile bool &pulsoCarro = (id == 1 ? pulsoCarro1 : pulsoCarro2);
  volatile bool &vengoDePeaton = (id == 1 ? vengoDePeaton1 : vengoDePeaton2);
  volatile unsigned long &lastChange = (id == 1 ? lastChange1 : lastChange2);
  volatile int &lastButtonState = (id == 1 ? lastButtonState1 : lastButtonState2);
  volatile unsigned long &lastButtonBounce = (id == 1 ? lastButtonBounce1 : lastButtonBounce2);
  volatile bool &buttonHeld = (id == 1 ? buttonHeld1 : buttonHeld2);
  volatile unsigned long &lastButtonPressMillis = (id == 1 ? lastButtonPressMillis1 : lastButtonPressMillis2);

  // Referencias al OTRO semáforo (para coordinar)
  State &otherState            = (id == 1 ? state2 : state1);
  bool  &otherSolicitudPeaton  = (id == 1 ? solicitudPeaton2 : solicitudPeaton1);
  bool  &otherPulsoCarro       = (id == 1 ? pulsoCarro2 : pulsoCarro1);

  int buttonPin = (id == 1 ? buttonPin1 : buttonPin2);
  int carPin    = (id == 1 ? carPin1    : carPin2);

  // ---------------- Lectura botón (nivel) ----------------
  int rawButton = digitalRead(buttonPin);

  if (rawButton != lastButtonState) lastButtonBounce = now;
  if (rawButton == LOW && (now - lastButtonBounce) > DEBOUNCE_MS) {
    if (!solicitudPeaton && !buttonHeld) {
      solicitudPeaton = true;
      buttonHeld = true;
      lastButtonPressMillis = now;
      Serial.printf("BUTTON %d: solicitud peaton\n", id);
    }
  }
  if (rawButton == HIGH) buttonHeld = false;
  lastButtonState = rawButton;

  // ---------------- Lectura carro: flanco ----------------
  static int lastCarState[3] = {1,1,1};
  int car = digitalRead(carPin);

  if (lastCarState[id] == 1 && car == 0) {
    if (now - lastButtonPressMillis <= 120) {
      Serial.printf("CAR %d: pulso ignorado\n", id);
    } else {
      pulsoCarro = true;
      Serial.printf("CAR %d: pulso registrado\n", id);
    }
  }
  lastCarState[id] = car;

  // ---------- SI EL OTRO ESTÁ EN FASE PEATONAL ----------
  // Si pedOwner != 0 y este NO es el dueño, se queda SIEMPRE en VERDE
  // mientras dure la fase peatonal.
  if (pedOwner != 0 && pedOwner != id) {
    if (state != GREEN) {
      changeState(id, GREEN);
    }
    // ignoramos botones y carros en el "semáforo esclavo" mientras hay peatón en el otro
    return;
  }

  // ---------------- FSM ----------------
  switch (state) {

    case RED:
      // Prioridad: peatón propio, solo si NO hay otra fase peatonal activa
      if (solicitudPeaton && pedOwner == 0) {
        solicitudPeaton = false;
        vengoDePeaton = false;

        pedOwner = id;  // este semáforo es el dueño de la fase peatonal

        // Este semáforo entra a PEDESTRIAN
        changeState(id, PEDESTRIAN);

        // El otro semáforo se pone en VERDE y se mantendrá verde
        {
          int otherId = (id == 1 ? 2 : 1);
          changeState(otherId, GREEN);
        }

        break;
      }

      // Cambio anticipado por carro SOLO si:
      // - en el otro NO hay carro
      // - en el otro NO hay solicitud de peatón
      // - NO hay fase peatonal activa
      if (pulsoCarro &&
          !otherPulsoCarro &&
          !otherSolicitudPeaton &&
          pedOwner == 0) {

        pulsoCarro = false;
        vengoDePeaton = false;
        changeState(id, YELLOW_AFTER_RED);
        break;
      }

      // Cambio normal por tiempo, pero sin lanzar a VERDE al mismo tiempo que el otro
      if ((now - lastChange) >= RED_TIME && pedOwner == 0) {
        // Sólo pasamos a YELLOW si el otro NO está en verde ni en peatonal
        if (otherState != GREEN && otherState != PEDESTRIAN) {
          changeState(id, YELLOW_AFTER_RED);
        }
      }
      break;

    case YELLOW_AFTER_RED:
      // Carro puede acelerar el paso a verde, con las mismas condiciones de arriba
      if (pulsoCarro &&
          !otherPulsoCarro &&
          !otherSolicitudPeaton &&
          pedOwner == 0) {

        pulsoCarro = false;
        vengoDePeaton = false;

        // Nunca permitir dos verdes: solo cambiamos si el otro no está verde/peatón
        if (otherState != GREEN && otherState != PEDESTRIAN) {
          changeState(id, GREEN);
        }
        break;
      }

      if ((now - lastChange) >= YELLOW_TIME && pedOwner == 0) {
        // Transición normal a verde, pero evitando doble verde
        if (otherState != GREEN && otherState != PEDESTRIAN) {
          changeState(id, GREEN);
        }
      }
      break;

    case GREEN:
      if (pulsoCarro) {
        pulsoCarro = false;
        Serial.printf("CAR %d: ignorado en GREEN\n", id);
      }
      if ((now - lastChange) >= GREEN_TIME && pedOwner == 0) {
        changeState(id, YELLOW_AFTER_GREEN);
      }
      break;

    case YELLOW_AFTER_GREEN:
      if ((now - lastChange) >= YELLOW_TIME) {
        if (vengoDePeaton) {
          vengoDePeaton = false;
          // Venimos de peatonal: volvemos a verde, pero igual respetando que el otro no esté en verde
          if (otherState != GREEN && otherState != PEDESTRIAN) {
            changeState(id, GREEN);
          } else {
            // Si el otro está verde, esperamos quedándonos en rojo
            changeState(id, RED);
          }
        } else {
          changeState(id, RED);
        }
      }
      break;

    case PEDESTRIAN:
      // IMPORTANTE:
      // Si llega un carro mientras estamos en modo peatonal,
      // NO se rompe la secuencia del peatón.
      // Se ignora el pulso de carro en esta fase.
      if (pulsoCarro) {
        pulsoCarro = false;                    // lo limpiamos
        Serial.printf("CAR %d: ignorado en PEDESTRIAN\n", id);
      }

      // Fin de tiempo peatonal: se pasa a amarillo y luego seguirá lógica normal
      if ((now - lastChange) >= PED_TIME) {
        vengoDePeaton = true;
        solicitudPeaton = false;
        pedOwner = 0;   // termina fase peatonal global
        changeState(id, YELLOW_AFTER_GREEN);
      }
      break;
  }
}

// ==================== CAMBIO DE ESTADO ====================
void changeState(int id, State newState) {
  allOff(id);
  (id == 1 ? state1 : state2) = newState;
  (id == 1 ? lastChange1 : lastChange2) = millis();

  int red    = (id == 1 ? redPin1    : redPin2);
  int yellow = (id == 1 ? yellowPin1 : yellowPin2);
  int green  = (id == 1 ? greenPin1  : greenPin2);
  int ped    = (id == 1 ? pedPin1    : pedPin2);

  switch (newState) {
    case RED:
      digitalWrite(red, HIGH);
      Serial.printf("S%d -> RED\n", id);
      break;
    case YELLOW_AFTER_RED:
      digitalWrite(yellow, HIGH);
      Serial.printf("S%d -> YELLOW_R\n", id);
      break;
    case GREEN:
      digitalWrite(green, HIGH);
      Serial.printf("S%d -> GREEN\n", id);
      break;
    case YELLOW_AFTER_GREEN:
      digitalWrite(yellow, HIGH);
      Serial.printf("S%d -> YELLOW_G\n", id);
      break;
    case PEDESTRIAN:
      digitalWrite(red, HIGH);
      digitalWrite(ped, HIGH);
      Serial.printf("S%d -> PEDESTRIAN\n", id);
      break;
  }

  // Avisar a Blynk en qué estado está cada semáforo
  reportStateToBlynk(id, newState);
}

void allOff(int id) {
  digitalWrite((id == 1 ? redPin1    : redPin2), LOW);
  digitalWrite((id == 1 ? yellowPin1 : yellowPin2), LOW);
  digitalWrite((id == 1 ? greenPin1  : greenPin2), LOW);
  digitalWrite((id == 1 ? pedPin1    : pedPin2), LOW);
}

// ==================== LECTURA DE CONTADOR SÓLO S1 ====================
// Lee QA..QD del contador del semáforo 1 y devuelve un valor 0–15
int readCounter1() {
  int qa = digitalRead(cnt1_Q0);
  int qb = digitalRead(cnt1_Q1);
  int qc = digitalRead(cnt1_Q2);
  int qd = digitalRead(cnt1_Q3);

  // HIGH = 1, LOW = 0
  int b0 = (qa == HIGH) ? 1 : 0;  // QA = bit 0
  int b1 = (qb == HIGH) ? 1 : 0;  // QB = bit 1
  int b2 = (qc == HIGH) ? 1 : 0;  // QC = bit 2
  int b3 = (qd == HIGH) ? 1 : 0;  // QD = bit 3

  int value = (b3 << 3) | (b2 << 2) | (b1 << 1) | b0;  // 0–15

  return value;
}

// ==================== BLYNK: ESTADOS ====================
String stateToText(State s) {
  switch (s) {
    case RED:               return "RED";
    case YELLOW_AFTER_RED:  return "YELLOW_AFTER_RED";
    case GREEN:             return "GREEN";
    case YELLOW_AFTER_GREEN:return "YELLOW_AFTER_GREEN";
    case PEDESTRIAN:        return "PEDESTRIAN";
  }
  return "UNKNOWN";
}

void reportStateToBlynk(int id, State s) {
  String txt = stateToText(s);
  if (id == 1) {
    Blynk.virtualWrite(VP_STATE_S1, txt);
  } else {
    Blynk.virtualWrite(VP_STATE_S2, txt);
  }
}

// ==================== BLYNK: BOTONES VIRTUALES ====================

// Reset contador S1
BLYNK_WRITE(VP_RESET_CNT1) {
  int v = param.asInt();
  if (v == 1) {
    resetCounter1Pulse();
  }
}

// Reset contador S2
BLYNK_WRITE(VP_RESET_CNT2) {
  int v = param.asInt();
  if (v == 1) {
    resetCounter2Pulse();
  }
}

// Botón peatonal virtual S1
BLYNK_WRITE(VP_PED_BTN1) {
  int v = param.asInt();
  if (v == 1) {
    if (!solicitudPeaton1) {
      solicitudPeaton1 = true;
      Serial.println("BLYNK: Peaton virtual S1");
    }
  }
}

// Botón peatonal virtual S2
BLYNK_WRITE(VP_PED_BTN2) {
  int v = param.asInt();
  if (v == 1) {
    if (!solicitudPeaton2) {
      solicitudPeaton2 = true;
      Serial.println("BLYNK: Peaton virtual S2");
    }
  }
}

// ==================== PULSOS DE RESET ====================
void resetCounter1Pulse() {
  // Suponiendo reset activo en LOW
  digitalWrite(resetCnt1Pin, LOW);
  delay(10);
  digitalWrite(resetCnt1Pin, HIGH);
  Serial.println("RESET contador S1 (Blynk)");
}

void resetCounter2Pulse() {
  digitalWrite(resetCnt2Pin, LOW);
  delay(10);
  digitalWrite(resetCnt2Pin, HIGH);
  Serial.println("RESET contador S2 (Blynk)");
}