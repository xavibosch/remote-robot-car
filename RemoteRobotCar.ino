// === BROWNOUT OFF (evita reset per pic motors) ===
#include "soc/soc.h"               // Dona acces als registres interns del ESP32
#include "soc/rtc_cntl_reg.h"      // Conte el registre del detector de brownout (baixada de tensio)

// === LLIBRERIES ===
#include <WiFi.h>                  // Llibreria per connectar el ESP32 al WiFi
#include <PubSubClient.h>          // Llibreria per parlar amb el broker MQTT

// === WIFI ===
const char* ssid = "YOUR_WIFI_SSID";        // Nom de la xarxa WiFi
const char* password = "YOUR_WIFI_PASSWORD"; // Contrasenya de la xarxa

// === MQTT ===
const char* mqtt_server = "broker.emqx.io"; // Adreça del broker MQTT (servidor intermediari a Internet)
const int mqtt_port = 1883;                 // Port estandard del protocol MQTT
const char* topic_car = "casa/esp32/car";   // Canal (topic) on arriben totes les ordres del mobil

// === PINS MOTORS ===
#define IN1 27   // Pin que fa anar els motors drets cap ENDAVANT
#define IN2 26   // Pin que fa anar els motors drets cap ENRERE
#define IN3 25   // Pin que fa anar els motors esquerres cap ENDAVANT
#define IN4 33   // Pin que fa anar els motors esquerres cap ENRERE
#define ENA 32   // Pin PWM que controla la VELOCITAT dels motors drets (canal A)
#define ENB 14   // Pin PWM que controla la VELOCITAT dels motors esquerres (canal B)

// === PINS SENSOR + BUZZER ===
#define TRIG 18           // Pin que DISPARA el pols ultrasonic del sensor HC-SR04
#define ECHO 19           // Pin que REP l'eco del sensor (passa pel divisor de tensio 5V->3,3V)
#define BUZZER 15         // Pin positiu del buzzer (avis acustic)
#define OBSTACLE_CM 20    // Si la distancia es menor de 10 cm = obstacle
#define MIN_VALID_CM 2    // Lectures de menys de 2 cm = soroll electric, es descarten
#define BEEP_MS 2000      // El pip d'avis dura 2000 ms (2 segons)
#define CONFIRM_COUNT 3   // Calen 3 lectures seguides per confirmar que l'obstacle es real
#define BACKUP_MS 500     // El retroces automatic dura 500 ms (~5 cm de recorregut)

// === MQTT CLIENT ===
WiFiClient espClient;              // Connexio de xarxa basica (TCP) que usara el MQTT
PubSubClient client(espClient);    // Client MQTT construit sobre aquesta connexio

// === ESTAT ===
String currentCmd = "stop";       // Ordre que el cotxe esta executant ara mateix
int speed = 170;                  // Velocitat actual en PWM (de 0 a 255)
unsigned long beepUntil = 0;      // Moment del rellotge fins quan ha de pitar
unsigned long lastBeep = 0;       // Moment de l'ultim canvi so/silenci del pip
bool beeping = false;             // true = el buzzer esta sonant ara mateix
int obstacleCount = 0;            // Quantes lectures seguides han vist obstacle
unsigned long backupUntil = 0;    // Moment del rellotge fins quan dura el retroces automatic


// ========== MOTORS ==========

void applySpeed() {
  analogWrite(ENA, speed);   // Envia la velocitat (0-255) als motors drets
  analogWrite(ENB, speed);   // Envia la velocitat (0-255) als motors esquerres
}

void stopMotors() {
  // Tot LOW + PWM 0 = parats
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);   // Apaga les dues direccions dels motors drets
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);   // Apaga les dues direccions dels motors esquerres
  analogWrite(ENA, 0);     // Velocitat 0 al canal A (sense corrent)
  analogWrite(ENB, 0);     // Velocitat 0 al canal B (sense corrent)
}

void forward() {
  applySpeed();                                       // Posa la velocitat actual als dos canals
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);    // Motors drets: endavant SI, enrere NO
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);    // Motors esquerres: endavant SI, enrere NO
}

void backward() {
  applySpeed();                                       // Posa la velocitat actual
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);    // Motors drets: endavant NO, enrere SI
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);    // Motors esquerres: endavant NO, enrere SI
}

void turnLeft() {
  applySpeed();                                       // Posa la velocitat actual
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);    // Motors drets: endavant (empenyen)
  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);    // Motors esquerres: parats -> el cotxe gira a l'esquerra
}

void turnRight() {
  applySpeed();                                       // Posa la velocitat actual
  digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);    // Motors drets: parats
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);    // Motors esquerres: endavant (empenyen) -> gira a la dreta
}

void applyCmd(String cmd) {
  // Mira quina ordre hi ha guardada i crida la funcio de moviment que toca
  if      (cmd == "forward") forward();    // "forward" -> avançar
  else if (cmd == "back")    backward();   // "back" -> enrere
  else if (cmd == "left")    turnLeft();   // "left" -> girar esquerra
  else if (cmd == "right")   turnRight();  // "right" -> girar dreta
  else                       stopMotors(); // qualsevol altra cosa ("stop") -> parar
}


// ========== HC-SR04 ==========

long readDistanceCm() {
  // Pols trigger 10us
  digitalWrite(TRIG, LOW);        // Comença amb el trigger apagat
  delayMicroseconds(2);           // Espera 2 microsegons (estabilitzar)
  digitalWrite(TRIG, HIGH);       // Activa el trigger...
  delayMicroseconds(10);          // ...durant 10 microsegons (el que demana el sensor)
  digitalWrite(TRIG, LOW);        // Apaga el trigger -> el sensor llança els ultrasons

  long duration = pulseIn(ECHO, HIGH, 20000);   // Mesura quant temps triga a tornar l'eco (maxim 20ms)
  if (duration == 0) return 999;                // Si no torna res = cap obstacle a prop -> retorna 999
  return duration * 0.034 / 2;                  // Converteix el temps a centimetres (velocitat del so, anada i tornada)
}


// ========== BUZZER ==========

void updateBeep() {
  // Pip intermitent cada 150ms
  unsigned long now = millis();          // Llegeix el rellotge intern (ms des de l'arrencada)
  if (now - lastBeep > 150) {            // Han passat mes de 150 ms des de l'ultim canvi?
    lastBeep = now;                      // Guarda el moment d'aquest canvi
    if (beeping) { noTone(BUZZER); beeping = false; }   // Si sonava -> el silencia
    else         { tone(BUZZER, 1500); beeping = true; } // Si callava -> so de 1500 Hz (aixi fa pip-pip-pip)
  }
}

void stopBeep() {
  if (beeping) { noTone(BUZZER); beeping = false; }   // Si el buzzer sona, el talla i actualitza l'estat
}


// ========== MQTT ==========

void callback(char* topic, byte* payload, unsigned int length) {
  // Aquesta funcio s'executa SOLA cada cop que arriba un missatge del broker
  String msg = "";                                          // Text buit per construir el missatge
  for (int i = 0; i < length; i++) msg += (char)payload[i]; // Converteix els bytes rebuts en text llegible
  msg.trim();          // Treu espais del principi i del final
  msg.toLowerCase();   // Ho passa tot a minuscules (aixi "STOP" i "stop" valen igual)

  Serial.println("MQTT: " + msg);   // Mostra el missatge rebut al Serial Monitor

  // VELOCITAT + AVANÇAR EN UN SOL COMANDO (cada ordre fixa velocitat i posa el cotxe en marxa)
  if (msg == "slow")   { speed = 100; currentCmd = "forward"; obstacleCount = 0; backupUntil = 0; Serial.println("Forward speed 100"); return; }   // Avança LENT
  if (msg == "normal") { speed = 170; currentCmd = "forward"; obstacleCount = 0; backupUntil = 0; Serial.println("Forward speed 170"); return; }   // Avança a velocitat MITJANA
  if (msg == "fast")   { speed = 220; currentCmd = "forward"; obstacleCount = 0; backupUntil = 0; Serial.println("Forward speed 220"); return; }   // Avança RAPID
  if (msg == "turbo")  { speed = 255; currentCmd = "forward"; obstacleCount = 0; backupUntil = 0; Serial.println("Forward speed 255"); return; }   // Avança al MAXIM

  // forward = avançar velocitat normal
  if (msg == "forward") {
    speed = 170;                          // Fixa la velocitat mitjana
    currentCmd = "forward";               // Guarda l'ordre d'avançar
    obstacleCount = 0;                    // Reinicia el comptador d'obstacles
    backupUntil = 0;                      // Cancela marxa enrere automatica si n'hi havia
    Serial.println("Forward speed 170");  // Confirmacio al Serial
    return;                               // Surt: ordre processada
  }

  // ALTRES DIRECCIONS
  if (msg == "back" || msg == "left" || msg == "right" || msg == "stop") {
    currentCmd = msg;       // Guarda la nova ordre (el loop l'executara)
    obstacleCount = 0;      // Reinicia el comptador d'obstacles
    backupUntil = 0;        // Ordre manual cancela auto-backup
  }
}

void reconnect() {
  while (!client.connected()) {                                  // Repeteix fins estar connectat al broker
    Serial.print("MQTT connect...");                             // Avis al Serial
    String clientId = "ESP32Car-" + String(random(0xffff), HEX); // Crea un nom unic aleatori per identificar-se
    if (client.connect(clientId.c_str())) {                      // Prova de connectar al broker
      Serial.println("OK");                                      // Connectat!
      client.subscribe(topic_car);                               // S'apunta al topic per rebre les ordres
    } else {                                                     // Si ha fallat...
      Serial.print("fail rc="); Serial.println(client.state());  // Mostra el codi d'error
      delay(3000);                                               // Espera 3 segons i ho torna a provar
    }
  }
}


// ========== SETUP ==========

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // Apaga el detector de brownout (evita reinicis pels pics dels motors)

  Serial.begin(115200);                        // Engega el Serial Monitor a 115200 bauds

  // Pins motors
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);  // IN1 i IN2 com a sortides
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);  // IN3 i IN4 com a sortides
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);  // ENA i ENB (velocitat) com a sortides
  stopMotors();                                // Comença amb el cotxe ben parat

  // Sensor + buzzer
  pinMode(TRIG, OUTPUT);                       // TRIG es sortida (el ESP32 dispara)
  pinMode(ECHO, INPUT);                        // ECHO es entrada (el ESP32 escolta)
  pinMode(BUZZER, OUTPUT);                     // Buzzer es sortida

  // WiFi
  WiFi.begin(ssid, password);                  // Comença la connexio al WiFi del mobil
  Serial.print("WiFi");                        // Escriu "WiFi" al Serial
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }   // Mentre no connecti: espera i posa punts
  Serial.println(" OK");                       // WiFi connectat

  // MQTT
  client.setServer(mqtt_server, mqtt_port);    // Diu al client quin broker i port usar
  client.setCallback(callback);                // Diu quina funcio executar quan arribi un missatge
}


// ========== LOOP ==========

void loop() {
  if (!client.connected()) reconnect();   // Si s'ha perdut la connexio MQTT, reconnecta
  client.loop();                          // Revisa si han arribat missatges nous (imprescindible)

  // === FASE 1: marxa enrere automatica en curs? ===
  if (millis() < backupUntil) {           // Encara estem dins del temps de retroces?
    // Esta retrocedint per apartar-se de l'obstacle
    backward();      // Continua anant enrere a la velocitat actual
    updateBeep();    // Mante el pip sonant mentre recula
    delay(20);       // Petita pausa
    return;          // Surt del loop: no fa res mes durant el backup
  }

  // Si el backup acaba de finalitzar, queda en stop
  // (currentCmd ja es "stop" des de la deteccio)

  // === FASE 2: deteccio obstacle (nomes avançant o girant) ===
  bool moving = (currentCmd == "forward" ||    // El sensor vigila si va endavant...
                 currentCmd == "left"    ||    // ...o gira a l'esquerra...
                 currentCmd == "right");       // ...o gira a la dreta (mai enrere ni parat)

  if (moving) {                                // Nomes si s'esta movent:
    long dist = readDistanceCm();              // Llegeix la distancia frontal en cm

    // Lectura amb obstacle valid (entre 2 i 10 cm)
    if (dist >= MIN_VALID_CM && dist < OBSTACLE_CM) {
      obstacleCount++;                         // Una lectura mes que veu obstacle
    } else {
      obstacleCount = 0;                       // Lectura neta: reinicia el comptador
    }

    // 3 lectures seguides = obstacle real
    if (obstacleCount >= CONFIRM_COUNT) {
      currentCmd = "stop";                    // Despres del backup quedara parat
      obstacleCount = 0;                      // Reinicia el comptador
      beepUntil = millis() + BEEP_MS;         // Programa el pip: 2 segons a partir d'ara
      backupUntil = millis() + BACKUP_MS;     // Programa el retroces: 500 ms a partir d'ara
      Serial.print("OBSTACLE "); Serial.print(dist);   // Avisa al Serial amb la distancia
      Serial.println("cm -> BACKUP + PIP");            // I diu que fara backup i pip
    }
  } else {
    obstacleCount = 0;                        // Si no es mou, no acumula deteccions
  }

  // === FASE 3: pip per temporitzador ===
  if (millis() < beepUntil) {     // Encara dins del temps de pip?
    updateBeep();                 // Continua el pip intermitent
  } else {
    stopBeep();                   // Temps esgotat: silenci
  }

  // === FASE 4: executa ordre actual ===
  applyCmd(currentCmd);           // Mou el cotxe segons l'ordre vigent (s'executa sempre)

  delay(50);                      // Espera 50 ms -> el loop es repeteix ~20 cops per segon
}
