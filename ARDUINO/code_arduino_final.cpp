/*
 * MOSH_Capteur — Projet 4GP INSA Toulouse
 * Reconstruit à partir de mes propres briques :
 *   - potentiometre_digital/tp4_F.ino     (MCP41050 SPI)
 *   - ecran_oled/tp4_ecran.ino            (SSD1306 I2C)
 *   - flex_sensor/tp3d.cpp                (flex + map)
 *   - encodeur_rotatif_interrupt/tp3g.ino (encodeur par interrupt)
 *   - bluetooth/tp1e.ino                  (SoftwareSerial HC-05)
 *   - blink_without_delay/tp1b.ino        (timing non-bloquant)
 *
 */

#include <SPI.h>  // pour piloter le potentiomètre digital (MCP41050) via SPI
#include <Wire.h>  //pilote le bus I2C (broches A4 SDA, A5 SCL) pour l'écran OLED
#include <SoftwareSerial.h>
#include <Adafruit_SSD1306.h> // driver haut niveau pour l'écran OLED

// ==============================================================
//  CONSTANTES — OLED (style ecran_oled/tp4_ecran.ino)
// ==============================================================
#define nombreDePixelsEnLargeur  128 //instancier l ecran oled avec resoltion
#define nombreDePixelsEnHauteur  64  //instancier l ecran oled avec resoltion
#define brocheResetOLED          -1 //pas de broche reset dédiée
#define adresseI2CecranOLED      0x3C //adresse I2C

Adafruit_SSD1306 ecranOLED(nombreDePixelsEnLargeur, nombreDePixelsEnHauteur, &Wire, brocheResetOLED);

// ==============================================================
//  CONSTANTES — POTENTIOMÈTRE DIGITAL (tp4_F.ino)
// ==============================================================
const byte csPin        = 10; // est la broche Chip Select : quand on la met à LOW, il écoute le bus SPI, quand elle est HIGH il ignore les données
const int  maxPositions = 256; // le potentiomètre numérique à 256 positions
const long rAB          = 92500;   // ajusté au multimètre
const byte rWiper       = 125;  //résistance résiduelle du curseur interne quand la position est à 0
const byte pot0         = 0x11;    // adresse potentiomètre 0

// ==============================================================
//  CONSTANTES — FLEX SENSOR (tp3d.cpp)
// ==============================================================
const int   flexPin         = A1;
const float VCC             = 5.0;
const float R_DIV           = 22000.0; // résistance de diviseur de tension
const float flatResistance  = 20300.0; //resistance en position plate mesuré par nous meme
const float bendResistance  = 50200.0; // resistance en potition pliée à 90 degrés mesuré par nous meme

// ==============================================================
//  CONSTANTES — ENCODEUR (tp3g.ino)
// ==============================================================
#define encoder0PinA  2    // CLK 
#define encoder0PinB  4    // DT
#define Switch        3    // bouton OK

volatile int encoder0Pos = 0; //est modifiée dans la routine d'interruption  
//Sans volatile, le compilateur pourrait optimiser en gardant une copie en registre et ne jamais relire la vraie valeur en RAM
int lastEncoderPos       = 0;

// ==============================================================
//  CONSTANTES — BLUETOOTH (tp1e.ino)
// ==============================================================
#define rxPin     8        // RX ← HC-05 TX
#define txPin     7        // TX → HC-05 RX
#define baudrate  9600

SoftwareSerial mySerial(rxPin, txPin);

// ==============================================================
//  CONSTANTES — CAPTEUR GRAPHITE
// ==============================================================
#define PIN_GRAPHITE  A0
#define R1            100000.0
#define R3            100000.0
#define R5            1000.0

// ==============================================================
//  ÉTAT GLOBAL
// ==============================================================
uint8_t potPos          = 128; //est la position actuelle du curseur du potentiomètre digital initialisée au milieu de la plage
int     menuIndex       = 0; //ligne sélectionnée dans le menu
unsigned long lastMeas  = 0; //on compare millis() - lastMeas à un intervalle plutôt que d'appeler delay() pour éviter de bloquer le programme

enum State { MENU, MESURE, MOYENNE, FLEX, CALIB };
State state = MENU;

// ==============================================================
//  BRIQUE — POTENTIOMÈTRE DIGITAL (tp4_F.ino)
// ==============================================================
void setPotWiper(int addr, int pos) {
  pos = constrain(pos, 0, 255);
  digitalWrite(csPin, LOW);
  SPI.transfer(addr);
  SPI.transfer(pos);
  digitalWrite(csPin, HIGH);
}

float potResistance(int pos) {
  return ((float)rAB * pos / maxPositions) + rWiper; // formule ligne 36 tp4_F.ino, convertit la position du potentiomètre en résistance équivalente
}

// ==============================================================
//  BRIQUE — CAPTEUR GRAPHITE
// ==============================================================
float readGraphiteV() { //lecture ADC 
  return analogRead(PIN_GRAPHITE) * VCC / 1023.0;
}

float readGraphiteAvg(int n) { // moyenne sur n periode et 10ms d'intervalle pour réduire le bruit
  long sum = 0;
  for (int i = 0; i < n; i++) { sum += analogRead(PIN_GRAPHITE); delay(10); }
  return (float)sum / (float)n * VCC / 1023.0;
}

float calcResGraphite(float vadc) {
  if (vadc < 0.001) return -1.0;  //Le test vadc < 0.001 protège contre la division par zéro, et r > 0 filtre les résultats physiquement impossibles
  float r2   = potResistance(potPos);
  float gain = 1.0 + R3 / r2;
  float r    = R1 * gain * (VCC / vadc) - R1 - R5;
  return (r > 0) ? r : -1.0;
}

// ==============================================================
//  BRIQUE — FLEX SENSOR (tp3d.cpp)
// ==============================================================
float readFlexR() {
  int   ADCflex = analogRead(flexPin);
  float Vflex   = ADCflex * VCC / 1023.0;
  if (Vflex <= 0.001) return -1.0;
  return R_DIV * (VCC / Vflex - 1.0);
}

float readFlexAngle() {
  float Rflex = readFlexR();
  if (Rflex < 0) return 0;
  float angle = map(Rflex, flatResistance, bendResistance, 0, 90);
  return constrain(angle, 0, 90);
}

// ==============================================================
//  BRIQUE — ENCODEUR (ISR de tp3g.ino) + bouton SW
// ==============================================================
void doEncoder() {
  if (digitalRead(encoder0PinA) == HIGH && digitalRead(encoder0PinB) == HIGH) {
    encoder0Pos++; //front montant de A, si B est HIGH, les deux signaux sont en phase ==> incrémentation
  } else if (digitalRead(encoder0PinA) == HIGH && digitalRead(encoder0PinB) == LOW) {
    encoder0Pos--;//front montant de A, si B est LOW, les deux signaux sont en opposition de phase ==> décrémentation
  }
}

bool readSwitch() { //Si moins de 300 ms se sont écoulées depuis le dernier appui détecté, on ignore l'appui
  static unsigned long lastSW = 0;
  if (millis() - lastSW > 300 && digitalRead(Switch) == LOW) {
    lastSW = millis();
    return true;
  }
  return false;
}

// ==============================================================
//  BRIQUE — BLUETOOTH (tp1e.ino)
// ==============================================================
void btSend(float val, const char* unit) {
  mySerial.print(val, 2);
  mySerial.print(' ');
  mySerial.println(unit);
}

// ==============================================================
//  AFFICHAGE OLED
// ==============================================================
void showMenu() {
  const char* labels[] = {"Mesure instant.", "Mesure moyennee", "Flex sensor", "Calibration pot"};
  ecranOLED.clearDisplay();
  ecranOLED.setTextSize(1); ecranOLED.setTextColor(SSD1306_WHITE);
  ecranOLED.setCursor(0, 0); ecranOLED.print(F("== MENU =="));
  for (int i = 0; i < 4; i++) {
    ecranOLED.setCursor(0, 14 + i * 12);
    ecranOLED.print(i == menuIndex ? F("> ") : F("  "));
    ecranOLED.print(labels[i]);
  }
  ecranOLED.display();
}

void showGraphite(float r) {
  ecranOLED.clearDisplay();
  ecranOLED.setTextSize(1); ecranOLED.setTextColor(SSD1306_WHITE);
  ecranOLED.setCursor(0, 0);  ecranOLED.print(F("== GRAPHITE =="));
  ecranOLED.setCursor(0, 16); ecranOLED.print(F("R = "));
  ecranOLED.setTextSize(2);
  if (r < 0)         { ecranOLED.print(F("ERR")); }
  else if (r > 1e6)  { ecranOLED.print(r / 1e6, 2); ecranOLED.setTextSize(1); ecranOLED.print(F(" MOhm")); }
  else if (r > 1e3)  { ecranOLED.print(r / 1e3, 2); ecranOLED.setTextSize(1); ecranOLED.print(F(" kOhm")); }
  else               { ecranOLED.print(r, 0);       ecranOLED.setTextSize(1); ecranOLED.print(F(" Ohm"));  }
  ecranOLED.setTextSize(1);
  ecranOLED.setCursor(0, 40); ecranOLED.print(F("Pot: ")); ecranOLED.print(potPos); ecranOLED.print(F("/255"));
  ecranOLED.setCursor(0, 56); ecranOLED.print(F("[Clic] = retour"));
  ecranOLED.display();
}

void showFlex(float r, float angle) {
  ecranOLED.clearDisplay();
  ecranOLED.setTextSize(1); ecranOLED.setTextColor(SSD1306_WHITE);
  ecranOLED.setCursor(0, 0);  ecranOLED.print(F("== FLEX SENSOR =="));
  ecranOLED.setCursor(0, 16); ecranOLED.print(F("R = ")); ecranOLED.print(r / 1000.0, 1); ecranOLED.print(F(" kOhm"));
  ecranOLED.setCursor(0, 30); ecranOLED.print(F("Angle: "));
  ecranOLED.setTextSize(2); ecranOLED.print(angle, 1); ecranOLED.setTextSize(1); ecranOLED.print(F(" deg"));
  ecranOLED.setCursor(0, 56); ecranOLED.print(F("[Clic] = retour"));
  ecranOLED.display();
}

void showCalib() {
  ecranOLED.clearDisplay();
  ecranOLED.setTextSize(1); ecranOLED.setTextColor(SSD1306_WHITE);
  ecranOLED.setCursor(0, 0);  ecranOLED.print(F("== CALIBRATION =="));
  ecranOLED.setCursor(0, 16); ecranOLED.print(F("Pot: "));
  ecranOLED.setTextSize(2); ecranOLED.print(potPos); ecranOLED.setTextSize(1); ecranOLED.print(F(" /255"));
  ecranOLED.setCursor(0, 38); ecranOLED.print(F("Gain ampli:"));
  int w = map(potPos, 0, 255, 0, 120);
  ecranOLED.drawRect(0, 48, 124, 10, SSD1306_WHITE);
  ecranOLED.fillRect(2, 50, w, 6, SSD1306_WHITE);
  ecranOLED.setCursor(0, 60); ecranOLED.print(F("[Clic]=retour"));
  ecranOLED.display();
}

// ==============================================================
//  CALIBRATION AUTO (recherche coarse + fine sur setPotWiper)
// ==============================================================
void autoCalibrate() {
  ecranOLED.clearDisplay();
  ecranOLED.setCursor(0, 0); ecranOLED.print(F("Calibration auto..."));
  ecranOLED.display();

  int bestPos = 128, bestDelta = 9999;

  for (int p = 0; p < 256; p += 4) {
    setPotWiper(pot0, p); delay(30);
    int d = abs(analogRead(PIN_GRAPHITE) - 512);
    if (d < bestDelta) { bestDelta = d; bestPos = p; }
  }
  for (int p = max(0, bestPos - 4); p <= min(255, bestPos + 4); p++) {
    setPotWiper(pot0, p); delay(30);
    int d = abs(analogRead(PIN_GRAPHITE) - 512);
    if (d < bestDelta) { bestDelta = d; bestPos = p; }
  }

  potPos = bestPos;
  setPotWiper(pot0, potPos);

  ecranOLED.clearDisplay();
  ecranOLED.setCursor(0, 0);  ecranOLED.print(F("Calibration OK"));
  ecranOLED.setCursor(0, 16); ecranOLED.print(F("Pot = ")); ecranOLED.print(potPos);
  ecranOLED.display();
  delay(1500);
}

// ==============================================================
//  SETUP
// ==============================================================
void setup() {
  Serial.begin(baudrate);
  mySerial.begin(baudrate);

  // SPI + pot
  digitalWrite(csPin, HIGH);
  pinMode(csPin, OUTPUT);
  SPI.begin();

  // Encodeur (style tp3g.ino)
  pinMode(encoder0PinA, INPUT); digitalWrite(encoder0PinA, HIGH);
  pinMode(encoder0PinB, INPUT); digitalWrite(encoder0PinB, HIGH);
  pinMode(Switch,       INPUT); digitalWrite(Switch,       HIGH);
  attachInterrupt(0, doEncoder, RISING);

  // OLED (style tp4_ecran.ino : halt si échec)
  if (!ecranOLED.begin(SSD1306_SWITCHCAPVCC, adresseI2CecranOLED))
    while (1);
  ecranOLED.setTextSize(1); ecranOLED.setTextColor(SSD1306_WHITE);

  autoCalibrate();
  showMenu();
}

// ==============================================================
//  LOOP — machine à états
// ==============================================================
void loop() {
  // Détection rotation : comparaison encoder0Pos (modifié par ISR) / lastEncoderPos
  int delta = 0;
  noInterrupts();
  int current = encoder0Pos;
  interrupts();
  if (current != lastEncoderPos) {
    delta = (current > lastEncoderPos) ? 1 : -1;
    lastEncoderPos = current;
  }

  bool ok = readSwitch();

  switch (state) {

    case MENU:
      if (delta < 0) { menuIndex = (menuIndex == 0) ? 3 : menuIndex - 1; showMenu(); }
      if (delta > 0) { menuIndex = (menuIndex + 1) % 4;                   showMenu(); }
      if (ok) {
        if (menuIndex == 0) state = MESURE;
        if (menuIndex == 1) state = MOYENNE;
        if (menuIndex == 2) state = FLEX;
        if (menuIndex == 3) { state = CALIB; showCalib(); }
        lastMeas = 0;
      }
      break;

    case MESURE:
      if (ok) { state = MENU; showMenu(); break; }
      if (millis() - lastMeas >= 500) {
        lastMeas = millis();
        float r = calcResGraphite(readGraphiteV());
        showGraphite(r);
        btSend(r > 1e6 ? r / 1e6 : r > 1e3 ? r / 1e3 : r,
               r > 1e6 ? "MOhm"  : r > 1e3 ? "kOhm"  : "Ohm");
        Serial.print(F("R=")); Serial.print(r);
        Serial.print(F(" Pot=")); Serial.println(potPos);
      }
      break;

    case MOYENNE:
      if (ok) { state = MENU; showMenu(); break; }
      if (millis() - lastMeas >= 2500) {
        lastMeas = millis();
        float r = calcResGraphite(readGraphiteAvg(5));
        showGraphite(r);
        btSend(r / 1e6, "MOhm");
      }
      break;

    case FLEX:
      if (ok) { state = MENU; showMenu(); break; }
      if (millis() - lastMeas >= 500) {
        lastMeas = millis();
        float r = readFlexR();
        showFlex(r, readFlexAngle());
        btSend(r, "Ohm");
      }
      break;

    case CALIB:
      if (ok) { state = MENU; showMenu(); break; }
      if (delta < 0 && potPos < 255) { potPos++; setPotWiper(pot0, potPos); showCalib(); }
      if (delta > 0 && potPos > 0)   { potPos--; setPotWiper(pot0, potPos); showCalib(); }
      break;
  }
}
