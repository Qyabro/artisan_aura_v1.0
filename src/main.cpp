/**
 * HARDWARE: TITAN Basic v1.0
 * FIRMWARE: v1.0
 * DESCRIPCION: Version con Wifi, USB y BT
 * * --- NOVEDADES v1.0 ---
 * 1. Red Wifi en modo AP
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

// --- VERSIONES ---
#define HARDWARE_VERSION "TITAN Basic v1.0"
#define FIRMWARE_VERSION "v1.0" 

// --- OBJETOS ---
ModbusIP mb;              
BluetoothSerial SerialBT; 
Preferences preferences;  

// --- CONFIGURACIÓN ---
#define DEFAULT_AP_SSID "TITAN-BASIC" 
#define DEFAULT_AP_PASS "12345678"      
#define DEFAULT_BT_NAME "TITAN-BASIC" 
#define DEFAULT_DNS     "titan"         

// --- PINES LEDS ---
#define PIN_LED_CONN 33  
#define PIN_LED_DATA 32  

// --- VARIABLES DE ESTADO ---
String wifi_mode = "AP";      
String wifi_ssid = "";        
String wifi_pass = "";        
String bt_name = "";          
String dns_name = "";         
bool wifi_is_running = false; 

// Variables para el parpadeo NO BLOQUEANTE del LED IO32
unsigned long ledDataTimer = 0; // Guarda cuándo se encendió el LED
bool ledDataState = false;      // Guarda si está prendido o apagado

// Variables para el parpadeo del LED Conexión
unsigned long lastBlinkTime = 0;
bool ledConnState = LOW;

// --- DIRECCIONES MODBUS ---
const int REG_TC1 = 101; //ET
const int REG_TC2 = 100; //BT
const int REG_TC3 = 102; //EXT1
const int REG_TC4 = 103; //EXT2

// --- VARIABLES TEMP ---
double g_tc1=0, g_tc2=0, g_tc3=0, g_tc4=0; 
double last_good_tc3 = 0.0; 
double last_good_tc4 = 0.0; 
bool unit_F = false;        

// --- PINES HARDWARE (FIJOS) ---
#define TC1_CS  23  
#define TC2_CS  22  
#define TC3_CS  16  
#define TC4_CS  4   
#define DUMMY_MOSI 17 

// --- FILTRO SMA ---
#define SMA 5
int sma_idx = 0;
bool sma_filled = false; 
double tc1s[SMA], tc2s[SMA], tc3s[SMA], tc4s[SMA]; 

// --- COMANDOS ---
char serialbt_cmds_buffer[128]; 
char serial_cmds_buffer[128];
SerialCommands serialbt_cmds(&SerialBT, serialbt_cmds_buffer, sizeof(serialbt_cmds_buffer), "\n", ";");
SerialCommands serial_cmds(&Serial, serial_cmds_buffer, sizeof(serial_cmds_buffer), "\n", ";");

// ==========================================
// SECCIÓN 1: GESTIÓN DE LEDS
// ==========================================

// 1. Activar LED Datos (Se llama desde Modbus o Comandos)
// Solo enciende la luz y anota la hora. NO usa delay().
void triggerTrafficLed() {
  digitalWrite(PIN_LED_DATA, HIGH);
  ledDataState = true;
  ledDataTimer = millis(); // Guardamos el momento exacto
}

// 2. Revisar si hay que apagar el LED Datos
// Se llama en el loop principal todo el tiempo
void checkTrafficLedOFF() {
  if (ledDataState) {
    // Si pasaron 50ms desde que se prendió, lo apagamos
    if (millis() - ledDataTimer > 50) { 
      digitalWrite(PIN_LED_DATA, LOW);
      ledDataState = false;
    }
  }
}

// 3. Callback Modbus (El Chivato)
uint16_t cbModbusRead(TRegister* reg, uint16_t val) {
  triggerTrafficLed(); // ¡Encender LED ya!
  return val;
}

// 4. LED Conexión (IO33)
void updateConnectionLed() {
  bool bt_connected = SerialBT.hasClient(); 
  bool wifi_connected = false;

  if (wifi_is_running) {
    if (wifi_mode == "AP") {
      if (WiFi.softAPgetStationNum() > 0) wifi_connected = true; 
    } 
    else if (wifi_mode == "STA") {
      if (WiFi.status() == WL_CONNECTED) wifi_connected = true;
    }
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
  delay(5); 
  double t4 = readWithRetry(TC4_CS);

  // Persistencia TC3/TC4
  if(!isnan(t3)) { last_good_tc3 = t3; } else { t3 = last_good_tc3; }
  if(!isnan(t4)) { last_good_tc4 = t4; } else { t4 = last_good_tc4; }

  // Filtro
  tc1s[sma_idx] = t1; tc2s[sma_idx] = t2; 
  tc3s[sma_idx] = t3; tc4s[sma_idx] = t4;

  sma_idx++; if(sma_idx >= SMA){ sma_filled = true; sma_idx = 0; } 

  if(sma_filled) {
      double a1=0, a2=0; int c1=0, c2=0;
      for(int i=0; i<SMA; i++) {
         if(!isnan(tc1s[i])) { a1+=tc1s[i]; c1++; }
         if(!isnan(tc2s[i])) { a2+=tc2s[i]; c2++; }
      }
      if(c1>0) g_tc1 = a1/c1;
      if(c2>0) g_tc2 = a2/c2;

      double sum3=0, sum4=0;
      for(int i=0; i<SMA; i++) { sum3 += tc3s[i]; sum4 += tc4s[i]; }
      g_tc3 = sum3 / SMA;
      g_tc4 = sum4 / SMA;
  } else {
      if(!isnan(t1)) g_tc1 = t1;
      if(!isnan(t2)) g_tc2 = t2;
      g_tc3 = t3; g_tc4 = t4; 
  }

  // Modbus Update
  if (wifi_is_running) {
    mb.Hreg(REG_TC1, g_tc1*10); 
    mb.Hreg(REG_TC2, g_tc2*10);
    mb.Hreg(REG_TC3, g_tc3*10);
    mb.Hreg(REG_TC4, g_tc4*10);
  }
}

// ==========================================
// SECCIÓN 3: COMANDOS
// ==========================================

void sendTC4Data(Stream* port) {
  triggerTrafficLed(); // Usamos la nueva función trigger
  port->print("0.00,"); 
  double t1 = g_tc1; double t2 = g_tc2; double t3 = g_tc3; double t4 = g_tc4;
  if(unit_F){
    t1 = t1 * 1.8 + 32; t2 = t2 * 1.8 + 32;
    t3 = t3 * 1.8 + 32; t4 = t4 * 1.8 + 32;
  }
  port->print(t1); port->print(","); port->print(t2); port->print(",");
  port->print(t3); port->print(","); port->print(t4);
  port->println(",0.00,0.00,0.00"); 
}

void cmdRead(SerialCommands* sender){ sendTC4Data(sender->GetSerial()); }
void cmdUnits(SerialCommands* sender){ 
  char* u = sender->Next(); if(u && u[0]=='F') unit_F=true; else unit_F=false; sender->GetSerial()->println("#OK"); 
}
void cmdFirmware(SerialCommands* sender){ sender->GetSerial()->println(FIRMWARE_VERSION); }
void cmdHardware(SerialCommands* sender){ sender->GetSerial()->println(HARDWARE_VERSION); }
void cmdBTName(SerialCommands* sender){ Stream* s = sender->GetSerial(); s->print("BT NAME: "); s->println(bt_name); }
void cmdDNSName(SerialCommands* sender){ Stream* s = sender->GetSerial(); s->print("DNS NAME: "); s->println(dns_name); }

void cmdTemp(SerialCommands* sender){
  triggerTrafficLed(); 
  Stream* s = sender->GetSerial();
  s->print("T1: "); s->print(g_tc1); s->print(unit_F ? "F" : "C");
  s->print(" | T2: "); s->print(g_tc2); 
  s->print(" | T3: "); s->print(g_tc3); 
  s->print(" | T4: "); s->println(g_tc4);
}

void cmdIP(SerialCommands* sender){
  Stream* s = sender->GetSerial(); s->print("MODE: "); s->print(wifi_mode); s->print(", IP: ");
  if(wifi_is_running){ if(wifi_mode == "AP") s->println(WiFi.softAPIP()); else s->println(WiFi.localIP()); } else { s->println("NONE"); }
}

// --- COMANDO: SIGNAL (RSSI) - Nivel de señal Wifi ---
void cmdSignal(SerialCommands* sender){
  Stream* s = sender->GetSerial();
  s->println("--- SIGNAL INFO ---");
  
  if(wifi_is_running) {
    if(wifi_mode == "STA") {
      long rssi = WiFi.RSSI();
      s->print("WIFI (Router): "); s->print(rssi); s->println(" dBm");
      if(rssi > -50) s->println("CALIDAD: EXCELENTE");
      else if(rssi > -70) s->println("CALIDAD: BUENA");
      else if(rssi > -80) s->println("CALIDAD: REGULAR");
      else s->println("CALIDAD: MALA (Posible lentitud)");
    } else {
      s->println("WIFI (AP): N/A (Eres el emisor)");
    }
  } else {
    s->println("WIFI: OFF");
  }
  
  s->println("BT: RSSI No disponible (Limitacion Libreria)");
  s->println("-------------------");
}

void cmdHelp(SerialCommands* sender){
  Stream* s = sender->GetSerial(); s->println("\n--- COMANDOS ---"); 
  s->println("READ, TEMP, IP, BT, DNS, STATUS");
  s->println("SIGNAL          : Ver potencia WiFi"); // Agregado
  s->println("SETWIFI;AP | OFF | STA;S;P"); 
  s->println("SETDNS;NAME | SETBTNAME;NAME"); s->println("----------------");
}

void cmdStatus(SerialCommands* sender){
  Stream* s = sender->GetSerial(); s->println("\n--- TITAN STATUS ---"); s->print("FW: "); s->println(FIRMWARE_VERSION);
  s->print("BT NAME:   "); s->println(bt_name); if(SerialBT.hasClient()) s->println("BT: CONECTADO"); else s->println("BT: ESPERANDO");
  s->print("WIFI MODE: "); s->println(wifi_mode);
  if(wifi_is_running){
    if(wifi_mode == "AP"){ s->print("AP CLIENTS: "); s->println(WiFi.softAPgetStationNum()); }
    else if(WiFi.status() == WL_CONNECTED) { 
      s->print("ROUTER: "); s->print(wifi_ssid); 
      s->print(" ("); s->print(WiFi.RSSI()); s->println(" dBm)"); // Agregado RSSI aquí también
    }
    s->print("IP:  "); if(wifi_mode == "AP") s->println(WiFi.softAPIP()); else s->println(WiFi.localIP());
    s->print("DNS: "); s->print(dns_name); s->println(".local");
  } else { s->println("WIFI: OFF"); } s->println("--------------------");
}

void cmdSetBtName(SerialCommands* sender){
  char* n = sender->Next(); if (n) { preferences.begin("titan-cfg", false); preferences.putString("btname", n); preferences.end(); sender->GetSerial()->println("#OK RESTART"); delay(500); ESP.restart(); }
}
void cmdSetDNS(SerialCommands* sender){
  char* n = sender->Next(); if (n) { preferences.begin("titan-cfg", false); preferences.putString("dns", n); preferences.end(); sender->GetSerial()->println("#OK RESTART"); delay(500); ESP.restart(); }
}
void cmdSetWifi(SerialCommands* sender){
  char* m = sender->Next(); if (!m) return; preferences.begin("titan-cfg", false);
  if (strcmp(m, "OFF") == 0) { preferences.putString("mode", "OFF"); sender->GetSerial()->println("#OK OFF"); }
  else if (strcmp(m, "AP") == 0) { preferences.putString("mode", "AP"); sender->GetSerial()->println("#OK AP"); }
  else if (strcmp(m, "STA") == 0) { char* s = sender->Next(); char* p = sender->Next(); if(s && p) { preferences.putString("mode", "STA"); preferences.putString("ssid", s); preferences.putString("pass", p); sender->GetSerial()->println("#OK STA"); } }
  preferences.end(); delay(500); ESP.restart(); 
}

// --- VINCULACIÓN COMANDOS ---
SerialCommand cmdReadUSB("READ", cmdRead);      SerialCommand cmdUnitsUSB("UNITS", cmdUnits);
SerialCommand cmdSetWifiUSB("SETWIFI", cmdSetWifi); SerialCommand cmdStatusUSB("STATUS", cmdStatus);
SerialCommand cmdSetBtNameUSB("SETBTNAME", cmdSetBtName); SerialCommand cmdFirUSB("FIR", cmdFirmware);
SerialCommand cmdHarUSB("HAR", cmdHardware); SerialCommand cmdTempUSB("TEMP", cmdTemp);
SerialCommand cmdHelpUSB("HELP", cmdHelp); SerialCommand cmdIPUSB("IP", cmdIP);
SerialCommand cmdBTUSB("BT", cmdBTName); SerialCommand cmdSetDNSUSB("SETDNS", cmdSetDNS);
SerialCommand cmdGetDNSUSB("DNS", cmdDNSName); SerialCommand cmdSignalUSB("SIGNAL", cmdSignal); // Nuevo

SerialCommand cmdReadBT("READ", cmdRead);      SerialCommand cmdUnitsBT("UNITS", cmdUnits);
SerialCommand cmdSetWifiBT("SETWIFI", cmdSetWifi); SerialCommand cmdStatusBT("STATUS", cmdStatus);
SerialCommand cmdSetBtNameBT("SETBTNAME", cmdSetBtName); SerialCommand cmdFirBT("FIR", cmdFirmware);
SerialCommand cmdHarBT("HAR", cmdHardware); SerialCommand cmdTempBT("TEMP", cmdTemp);
SerialCommand cmdHelpBT("HELP", cmdHelp); SerialCommand cmdIPBT("IP", cmdIP);
SerialCommand cmdBTBT("BT", cmdBTName); SerialCommand cmdSetDNSBT("SETDNS", cmdSetDNS);
SerialCommand cmdGetDNSBT("DNS", cmdDNSName); SerialCommand cmdSignalBT("SIGNAL", cmdSignal); // Nuevo

void cmdUnrecognized(SerialCommands* sender, const char* cmd){ }

// ==========================================
// SECCIÓN 4: SETUP
// ==========================================
void setup() {
  setCpuFrequencyMhz(160); 
  
  pinMode(PIN_LED_CONN, OUTPUT); pinMode(PIN_LED_DATA, OUTPUT);
  digitalWrite(PIN_LED_CONN, LOW); digitalWrite(PIN_LED_DATA, LOW);

  Serial.begin(115200);

  SPI.begin(18, 19, DUMMY_MOSI, 5); 
  pinMode(TC1_CS, OUTPUT); pinMode(TC2_CS, OUTPUT); pinMode(TC3_CS, OUTPUT); pinMode(TC4_CS, OUTPUT);
  digitalWrite(TC1_CS, HIGH); digitalWrite(TC2_CS, HIGH); digitalWrite(TC3_CS, HIGH); digitalWrite(TC4_CS, HIGH);

  for(int i=0; i<SMA; i++) { tc1s[i]=NAN; tc2s[i]=NAN; tc3s[i]=NAN; tc4s[i]=NAN; }

  preferences.begin("titan-cfg", true);
  wifi_mode = preferences.getString("mode", "AP"); 
  wifi_ssid = preferences.getString("ssid", "");
  wifi_pass = preferences.getString("pass", "");
  bt_name   = preferences.getString("btname", DEFAULT_BT_NAME); 
  dns_name  = preferences.getString("dns", DEFAULT_DNS); 
  preferences.end();

  SerialBT.begin(bt_name);

  if (wifi_mode == "STA") {
    WiFi.mode(WIFI_STA); WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    int r = 0; while(WiFi.status() != WL_CONNECTED && r < 10) { delay(500); r++; }
    if(WiFi.status() == WL_CONNECTED) { wifi_is_running = true; mb.server(); }
    else { WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS); wifi_is_running = true; wifi_mode = "AP (Fallback)"; mb.server(); }
  }
  else if(wifi_mode != "OFF") { 
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS); wifi_is_running = true; mb.server(); 
  }

  if (wifi_is_running) {
    if (MDNS.begin(dns_name.c_str())) MDNS.addService("modbus", "tcp", 502); 
    
    // --- VINCULAMOS CALLBACK A TODOS LOS REGISTROS ---
    // Así aseguramos que si Artisan lee el 100, 101, 102 o 103, el LED parpadee
    mb.onGetHreg(REG_TC1, cbModbusRead); 
    mb.onGetHreg(REG_TC2, cbModbusRead); 
    mb.onGetHreg(REG_TC3, cbModbusRead); 
    mb.onGetHreg(REG_TC4, cbModbusRead); 

    mb.addHreg(REG_TC1, 0); mb.addHreg(REG_TC2, 0); mb.addHreg(REG_TC3, 0); mb.addHreg(REG_TC4, 0);
  }

  serial_cmds.SetDefaultHandler(cmdUnrecognized); serialbt_cmds.SetDefaultHandler(cmdUnrecognized);
  serial_cmds.AddCommand(&cmdReadUSB); serial_cmds.AddCommand(&cmdUnitsUSB); serial_cmds.AddCommand(&cmdSetWifiUSB); 
  serial_cmds.AddCommand(&cmdStatusUSB); serial_cmds.AddCommand(&cmdSetBtNameUSB); serial_cmds.AddCommand(&cmdFirUSB); 
  serial_cmds.AddCommand(&cmdHarUSB); serial_cmds.AddCommand(&cmdTempUSB); serial_cmds.AddCommand(&cmdHelpUSB); 
  serial_cmds.AddCommand(&cmdIPUSB); serial_cmds.AddCommand(&cmdBTUSB); serial_cmds.AddCommand(&cmdSetDNSUSB); 
  serial_cmds.AddCommand(&cmdGetDNSUSB); serial_cmds.AddCommand(&cmdSignalUSB);
  
  serialbt_cmds.AddCommand(&cmdReadBT); serialbt_cmds.AddCommand(&cmdUnitsBT); serialbt_cmds.AddCommand(&cmdSetWifiBT); 
  serialbt_cmds.AddCommand(&cmdStatusBT); serialbt_cmds.AddCommand(&cmdSetBtNameBT); serialbt_cmds.AddCommand(&cmdFirBT); 
  serialbt_cmds.AddCommand(&cmdHarBT); serialbt_cmds.AddCommand(&cmdTempBT); serialbt_cmds.AddCommand(&cmdHelpBT); 
  serialbt_cmds.AddCommand(&cmdIPBT); serialbt_cmds.AddCommand(&cmdBTBT); serialbt_cmds.AddCommand(&cmdSetDNSBT); 
  serialbt_cmds.AddCommand(&cmdGetDNSBT); serialbt_cmds.AddCommand(&cmdSignalBT);
}

// ==========================================
// SECCIÓN 5: LOOP (MUY IMPORTANTE)
// ==========================================
unsigned long lastRead = 0;
void loop() {
  updateConnectionLed(); // 1. LED Verde (Conexión)
  checkTrafficLedOFF();  // 2. LED Rojo (Apagar si ya pasó el tiempo)
  
  if (wifi_is_running) mb.task(); 
  serial_cmds.ReadSerial(); 
  serialbt_cmds.ReadSerial(); 
  
  if(millis() - lastRead > 400) { 
    lastRead = millis(); 
    readTCs(); 
  } 
  delay(5);
}