/**
 * HARDWARE: AURA Basic v1.1
 * FIRMWARE: v1.4 (AP + Rele Dual: Serial, HTTP WebServer y Registro Modbus TCP)
 * DESCRIPCION: 3 Sensores, Wifi Fijo (AP), Salida Relé (Pin 4) controlada por HTTP URL y Registro Modbus 104
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>          
#include <SPI.h>              
#include <ModbusIP_ESP8266.h> 
#include <BluetoothSerial.h>  
#include <SerialCommands.h>   
#include <Preferences.h>      
#include "esp32-hal-cpu.h"    
#include <WebServer.h> 

// --- VERSIONES ---
#define HARDWARE_VERSION "TITAN Basic v1.1"
#define FIRMWARE_VERSION "v1.4" 

// --- OBJETOS ---
ModbusIP mb;              
BluetoothSerial SerialBT; 
Preferences preferences;  
WebServer server(80); 

// --- CONFIGURACIÓN WIFI (FIJO EN AP) ---
#define DEFAULT_AP_SSID "TITAN-BASIC" 
#define DEFAULT_AP_PASS "12345678"      
#define DEFAULT_BT_NAME "TITAN-BASIC" 
#define DEFAULT_DNS     "titan"         

// --- PINES LEDS ---
#define PIN_LED_CONN 33  
#define PIN_LED_DATA 32  

// --- VARIABLES DE ESTADO ---
String wifi_mode = "AP";      
String bt_name = "";          
String dns_name = "";         
bool wifi_is_running = false; 

unsigned long ledDataTimer = 0; 
bool ledDataState = false;      
unsigned long lastBlinkTime = 0;
bool ledConnState = LOW;

// --- DIRECCIONES MODBUS ---
const int REG_TC1 = 101; 
const int REG_TC2 = 100; 
const int REG_TC3 = 102; 
const int REG_TC4 = 103; 
const int REG_RELE = 104; // <-- NUEVO: Registro Modbus para el control del relé por WiFi

// --- VARIABLES TEMP ---
double g_tc1=0, g_tc2=0, g_tc3=0; 
double last_good_tc3 = 0.0; 
bool unit_F = false;        

// --- PINES HARDWARE ---
#define TC1_CS  23  
#define TC2_CS  22  
#define TC3_CS  16  
#define PIN_RELAY 4   
#define DUMMY_MOSI 17 

// --- FILTRO SMA ---
#define SMA 5
int sma_idx = 0;
bool sma_filled = false; 
double tc1s[SMA], tc2s[SMA], tc3s[SMA]; 

// --- COMANDOS ---
char serialbt_cmds_buffer[128]; 
char serial_cmds_buffer[128];
SerialCommands serialbt_cmds(&SerialBT, serialbt_cmds_buffer, sizeof(serialbt_cmds_buffer), "\n", ";");
SerialCommands serial_cmds(&Serial, serial_cmds_buffer, sizeof(serial_cmds_buffer), "\n", ";");

// ==========================================
// SECCIÓN 1: GESTIÓN DE LEDS
// ==========================================
void triggerTrafficLed() {
  digitalWrite(PIN_LED_DATA, HIGH);
  ledDataState = true;
  ledDataTimer = millis(); 
}

void checkTrafficLedOFF() {
  if (ledDataState) {
    if (millis() - ledDataTimer > 50) { 
      digitalWrite(PIN_LED_DATA, LOW);
      ledDataState = false;
    }
  }
}

uint16_t cbModbusRead(TRegister* reg, uint16_t val) {
  triggerTrafficLed(); 
  return val;
}

void updateConnectionLed() {
  bool bt_connected = SerialBT.hasClient(); 
  bool wifi_connected = false;

  if (wifi_is_running && WiFi.softAPgetStationNum() > 0) {
      wifi_connected = true; 
  }

  if (bt_connected) {
    digitalWrite(PIN_LED_CONN, HIGH); 
  }
  else if (wifi_connected) {
    if (millis() - lastBlinkTime > 500) { 
      lastBlinkTime = millis();
      ledConnState = !ledConnState; 
      digitalWrite(PIN_LED_CONN, ledConnState);
    }
  }
  else {
    digitalWrite(PIN_LED_CONN, LOW); 
  }
}

// ==========================================
// SECCIÓN 2: LECTURA HARDWARE
// ==========================================
double readRawSPI(uint8_t cs) {
  uint16_t v;
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);      
  delayMicroseconds(10);      
  v = SPI.transfer(0x00); v <<= 8; v |= SPI.transfer(0x00);    
  digitalWrite(cs, HIGH);     
  SPI.endTransaction();

  if (v & 0x4) {return NAN;} 
  v >>= 3; return v * 0.25; 
}

double readWithRetry(uint8_t cs) {
  double val = NAN;
  for(int i=0; i<3; i++) {
     val = readRawSPI(cs);
     if(!isnan(val)) return val; 
     delay(2); 
  }
  return NAN; 
}

void readTCs(){
  double t1 = readWithRetry(TC1_CS); 
  double t2 = readWithRetry(TC2_CS);
  delay(5); 
  double t3 = readWithRetry(TC3_CS); 

  if(!isnan(t3)) { last_good_tc3 = t3; } else { t3 = last_good_tc3; }

  tc1s[sma_idx] = t1; tc2s[sma_idx] = t2; tc3s[sma_idx] = t3; 

  sma_idx++; if(sma_idx >= SMA){ sma_filled = true; sma_idx = 0; } 

  if(sma_filled) {
      double a1=0, a2=0; int c1=0, c2=0;
      for(int i=0; i<SMA; i++) {
         if(!isnan(tc1s[i])) { a1+=tc1s[i]; c1++; }
         if(!isnan(tc2s[i])) { a2+=tc2s[i]; c2++; }
      }
      if(c1>0) g_tc1 = a1/c1;
      if(c2>0) g_tc2 = a2/c2;

      double sum3=0;
      for(int i=0; i<SMA; i++) { sum3 += tc3s[i]; }
      g_tc3 = sum3 / SMA;
  } else {
      if(!isnan(t1)) g_tc1 = t1;
      if(!isnan(t2)) g_tc2 = t2;
      g_tc3 = t3; 
  }

  if (wifi_is_running) {
    mb.Hreg(REG_TC1, g_tc1*10); 
    mb.Hreg(REG_TC2, g_tc2*10);
    mb.Hreg(REG_TC3, g_tc3*10);
    mb.Hreg(REG_TC4, 0); 
  }
}

// ==========================================
// SECCIÓN 3: COMANDOS SERIALES 
// ==========================================
void cmdRelay(SerialCommands* sender){
  char* state = sender->Next(); 
  if (state != NULL) {
    if (strcmp(state, "ON") == 0) {
      digitalWrite(PIN_RELAY, HIGH);
      mb.Hreg(REG_RELE, 1); // Sincroniza el registro Modbus
      sender->GetSerial()->println("#OK RELAY ACTIVADO");
    } 
    else if (strcmp(state, "OFF") == 0) {
      digitalWrite(PIN_RELAY, LOW);
      mb.Hreg(REG_RELE, 0); // Sincroniza el registro Modbus
      sender->GetSerial()->println("#OK RELAY DESACTIVADO");
    }
  }
}

void sendTC4Data(Stream* port) {
  triggerTrafficLed(); 
  port->print("0.00,"); 
  double t1 = g_tc1; double t2 = g_tc2; double t3 = g_tc3;
  if(unit_F){
    t1 = t1 * 1.8 + 32; t2 = t2 * 1.8 + 32; t3 = t3 * 1.8 + 32;
  }
  port->print(t1); port->print(","); port->print(t2); port->print(",");
  port->print(t3); port->println(",0.00,0.00,0.00,0.00"); 
}

void cmdRead(SerialCommands* sender){ sendTC4Data(sender->GetSerial()); }
void cmdUnits(SerialCommands* sender){ char* u = sender->Next(); if(u && u[0]=='F') unit_F=true; else unit_F=false; sender->GetSerial()->println("#OK"); }
void cmdFirmware(SerialCommands* sender){ sender->GetSerial()->println(FIRMWARE_VERSION); }
void cmdHardware(SerialCommands* sender){ sender->GetSerial()->println(HARDWARE_VERSION); }
void cmdStatus(SerialCommands* sender){
  Stream* s = sender->GetSerial(); 
  s->println("\n--- TITAN STATUS ---"); 
  s->print("FW: "); s->println(FIRMWARE_VERSION);
  s->print("WIFI MODE: "); s->println(wifi_mode);
  s->print("IP:  "); s->println(WiFi.softAPIP()); 
  s->print("RELE ESTADO: "); s->println(digitalRead(PIN_RELAY) ? "ON" : "OFF");
  s->println("--------------------");
}
void cmdUnrecognized(SerialCommands* sender, const char* cmd){ }

SerialCommand cmdReadUSB("READ", cmdRead);      
SerialCommand cmdUnitsUSB("UNITS", cmdUnits);
SerialCommand cmdFirUSB("FIR", cmdFirmware);
SerialCommand cmdHarUSB("HAR", cmdHardware); 
SerialCommand cmdStatusUSB("STATUS", cmdStatus);
SerialCommand cmdRelayUSB("RELAY", cmdRelay); 

SerialCommand cmdReadBT("READ", cmdRead);      
SerialCommand cmdUnitsBT("UNITS", cmdUnits);
SerialCommand cmdFirBT("FIR", cmdFirmware);
SerialCommand cmdHarBT("HAR", cmdHardware); 
SerialCommand cmdStatusBT("STATUS", cmdStatus);
SerialCommand cmdRelayBT("RELAY", cmdRelay); 

// ==========================================
// SECCIÓN 4: SETUP
// ==========================================
void setup() {
  setCpuFrequencyMhz(160); 
  
  pinMode(PIN_LED_CONN, OUTPUT); pinMode(PIN_LED_DATA, OUTPUT);
  digitalWrite(PIN_LED_CONN, LOW); digitalWrite(PIN_LED_DATA, LOW);

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW); 

  Serial.begin(115200);

  SPI.begin(18, 19, DUMMY_MOSI, 5); 
  pinMode(TC1_CS, OUTPUT); pinMode(TC2_CS, OUTPUT); pinMode(TC3_CS, OUTPUT); 
  digitalWrite(TC1_CS, HIGH); digitalWrite(TC2_CS, HIGH); digitalWrite(TC3_CS, HIGH); 

  for(int i=0; i<SMA; i++) { tc1s[i]=NAN; tc2s[i]=NAN; tc3s[i]=NAN; }

  preferences.begin("titan-cfg", true);
  bt_name   = preferences.getString("btname", DEFAULT_BT_NAME); 
  dns_name  = preferences.getString("dns", DEFAULT_DNS); 
  preferences.end();

  SerialBT.begin(bt_name);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS); 
  wifi_is_running = true; 
  mb.server(); 

  if (wifi_is_running) {
    if (MDNS.begin(dns_name.c_str())) MDNS.addService("modbus", "tcp", 502); 
    
    mb.onGetHreg(REG_TC1, cbModbusRead); 
    mb.onGetHreg(REG_TC2, cbModbusRead); 
    mb.onGetHreg(REG_TC3, cbModbusRead); 
    mb.onGetHreg(REG_TC4, cbModbusRead); // Ya no se usa
    
    mb.addHreg(REG_TC1, 0); 
    mb.addHreg(REG_TC2, 0); 
    mb.addHreg(REG_TC3, 0); 
    mb.addHreg(REG_TC4, 0); // Ya no se usa
    mb.addHreg(REG_RELE, 0); // <-- NUEVO: Inicializamos el registro 104 en 0 (Relé apagado de fábrica)

    // --- Ruteo de los enlaces HTTP ---
    server.on("/relay/on", []() {
      triggerTrafficLed();
      digitalWrite(PIN_RELAY, HIGH);
      mb.Hreg(REG_RELE, 1); // Sincroniza el registro Modbus
      server.send(200, "text/plain", "RELE ENCENDIDO");
    });
    
    server.on("/relay/off", []() {
      triggerTrafficLed();
      digitalWrite(PIN_RELAY, LOW);
      mb.Hreg(REG_RELE, 0); // Sincroniza el registro Modbus
      server.send(200, "text/plain", "RELE APAGADO");
    });
    
    server.begin(); 
  }

  serial_cmds.SetDefaultHandler(cmdUnrecognized); serialbt_cmds.SetDefaultHandler(cmdUnrecognized);
  serial_cmds.AddCommand(&cmdReadUSB); serial_cmds.AddCommand(&cmdUnitsUSB); 
  serial_cmds.AddCommand(&cmdStatusUSB); serial_cmds.AddCommand(&cmdFirUSB); 
  serial_cmds.AddCommand(&cmdHarUSB); serial_cmds.AddCommand(&cmdRelayUSB);
  
  serialbt_cmds.AddCommand(&cmdReadBT); serialbt_cmds.AddCommand(&cmdUnitsBT); 
  serialbt_cmds.AddCommand(&cmdStatusBT); serialbt_cmds.AddCommand(&cmdFirBT); 
  serialbt_cmds.AddCommand(&cmdHarBT); serialbt_cmds.AddCommand(&cmdRelayBT);
}

// ==========================================
// SECCIÓN 5: LOOP
// ==========================================
unsigned long lastRead = 0;
void loop() {
  updateConnectionLed(); 
  checkTrafficLedOFF();  
  
  if (wifi_is_running) {
    mb.task(); 
    server.handleClient(); 

    // --- NUEVA LÓGICA: Control del pin físico mediante Modbus TCP ---
    if (mb.Hreg(REG_RELE) == 1) {
      digitalWrite(PIN_RELAY, HIGH);
    } else if (mb.Hreg(REG_RELE) == 0) {
      digitalWrite(PIN_RELAY, LOW);
    }
  }
  
  serial_cmds.ReadSerial(); 
  serialbt_cmds.ReadSerial(); 
  
  if(millis() - lastRead > 400) { 
    lastRead = millis(); 
    readTCs(); 
  } 
  delay(5);
}