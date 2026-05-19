#include <WiFi.h>
#include <FirebaseESP32.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <MFRC522.h>

// ==========================================
// THÔNG TIN WIFI & FIREBASE CỦA BẠN
// ==========================================
#define WIFI_SSID "Dak" 
#define WIFI_PASSWORD "12345678"
#define FIREBASE_HOST "nhathongminh-7be59-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "i6bevHBgLYIanl7lmYBsPrPgyY2PsxEdabOpq2eN"

// --- INPUT (Cảm biến) ---
#define DHT_PIN       4
#define DHT_TYPE      DHT11
#define GAS_PIN       34
#define FIRE_PIN      36
#define RAIN_PIN      35
#define IR_DOOR_PIN   39
#define IR_WC_PIN     13

// --- OUTPUT (Đèn/Relay/Còi) ---
#define LED_PK        16 
#define LED_BEP       25 
#define LED_PN        27 
#define FAN_BEP       26 
#define FAN_PN        32 
#define AC_DEMO       21 
#define FAN_WC        17 
#define LED_WC        12 
#define LED_ALARM     33 
#define BUZZER_PIN    14 

// --- SERVO ---
#define SERVO_DOOR_PIN  15
#define SERVO_CLOTH_PIN 2

// --- RFID RC522 ---
#define SS_PIN        5
#define RST_PIN       22

DHT dht(DHT_PIN, DHT_TYPE);
Servo servoDoor;
Servo servoCloth;
MFRC522 rfid(SS_PIN, RST_PIN);

// Khai báo đối tượng Firebase 
FirebaseData fbdoSend;   
FirebaseData fbdoStream; 
FirebaseAuth auth;
FirebaseConfig config;

// Biến chạy đa nhiệm bằng millis()
unsigned long lastSensorTime = 0;
unsigned long lastAutoCheckTime = 0;
unsigned long personDetectedMillis = 0; 
unsigned long doorOpenMillis = 0;

// Các biến lưu trạng thái điều khiển & Tự động
int autoPhoi = 0;
int canhBaoPIR = 0;
int appCoiState = 0;
bool isWaitingForAlarm = false;

// Sửa thuật toán chống lặp lệnh Servo
bool isDoorOpenPhysically = false; // Biến chốt chặn trạng thái thực tế của cửa

// ==========================================
// HÀM LẮNG NGHE LỆNH TỪ APP (STREAM DATA)
// ==========================================
void streamCallback(StreamData data) {
  String path = data.dataPath();
  
  if (data.dataType() == "int") {
    int val = data.intData();
    
    // PHÒNG KHÁCH
    if (path == "/PhongKhach/Den") digitalWrite(LED_PK, val);
    else if (path == "/PhongKhach/DieuHoa") digitalWrite(AC_DEMO, val);
    
    // NHÀ BẾP
    else if (path == "/NhaBep/Den") {
      digitalWrite(LED_BEP, val);
      Serial.println("Den Bep: " + String(val));
    }
    else if (path == "/NhaBep/QuatHut") {
      digitalWrite(FAN_BEP, val);
      Serial.println("Quat Bep: " + String(val));
    }
    
    // PHÒNG NGỦ
    else if (path == "/PhongNgu/Den") digitalWrite(LED_PN, val);
    else if (path == "/PhongNgu/Quat") digitalWrite(FAN_PN, val);
    
    // BAN CÔNG
    else if (path == "/BanCong/GiaPhoi") servoCloth.write(val == 1 ? 90 : 0);
    else if (path == "/BanCong/AutoMode") autoPhoi = val;
    
    // AN NINH & CỬA CHÍNH
    else if (path == "/CuaChinh/CanhBao") canhBaoPIR = val;
    else if (path == "/CuaChinh/Coi") {
      appCoiState = val; 
      digitalWrite(BUZZER_PIN, val);
      digitalWrite(LED_ALARM, val);
    }
    else if (path == "/CuaChinh/Servo") {
      if (val == 90) {
        if (!isDoorOpenPhysically) { // Chỉ quay khi thực tế cửa đang đóng
          servoDoor.write(90);
          isDoorOpenPhysically = true;
          doorOpenMillis = millis(); // Ghi nhận thời điểm mở
          Serial.println("=> Mo cua tu xa tu APP.");
        }
      } else {
        if (isDoorOpenPhysically) { // Chỉ quay về khi thực tế cửa đang mở
          servoDoor.write(0);
          isDoorOpenPhysically = false;
          Serial.println("=> Dong cua tu xa tu APP.");
        }
      }
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Firebase Stream bi gian doan, dang ket noi lai...");
}

// ==========================================
// HÀM SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000); 

  pinMode(GAS_PIN, INPUT); 
  pinMode(FIRE_PIN, INPUT); 
  pinMode(RAIN_PIN, INPUT);
  pinMode(IR_DOOR_PIN, INPUT); 
  pinMode(IR_WC_PIN, INPUT);
  
  int outputs[] = {LED_PK, LED_BEP, LED_PN, FAN_BEP, FAN_PN, AC_DEMO, FAN_WC, LED_WC, LED_ALARM, BUZZER_PIN};
  for(int i = 0; i < 10; i++) {
    pinMode(outputs[i], OUTPUT);
    digitalWrite(outputs[i], LOW);
  }

  ESP32PWM::allocateTimer(0); 
  ESP32PWM::allocateTimer(1);
  servoDoor.setPeriodHertz(50); 
  servoCloth.setPeriodHertz(50);
  servoDoor.attach(SERVO_DOOR_PIN, 500, 2400); 
  servoCloth.attach(SERVO_CLOTH_PIN, 500, 2400);
  
  servoDoor.write(0); 
  servoCloth.write(0);
  isDoorOpenPhysically = false;
  
  dht.begin();
  SPI.begin(18, 19, 23, 5); 
  rfid.PCD_Init();

  Serial.print("Dang ket noi WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi OK!");

  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  
  fbdoSend.setBSSLBufferSize(4096, 1024);
  fbdoStream.setBSSLBufferSize(4096, 1024);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  Firebase.beginStream(fbdoStream, "/SmartHome");
  Firebase.setStreamCallback(fbdoStream, streamCallback, streamTimeoutCallback);
  
  Serial.println("HE THONG DA SAN SANG!");
}

// ==========================================
// HÀM LOOP
// ==========================================
void loop() {
  unsigned long currentMillis = millis();

  // 1. XỬ LÝ QUÉT THẺ TỪ RFID VÀ ĐỒNG BỘ 2 CHIỀU
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidStr = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uidStr += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      uidStr += String(rfid.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();
    Serial.println("=> QUET THE TỪ THÀNH CÔNG: " + uidStr);
    
    digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW);
    
    // Kích hoạt mở cửa vật lý
    servoDoor.write(90); 
    isDoorOpenPhysically = true;
    doorOpenMillis = currentMillis; // Đặt mốc thời gian bắt đầu mở cửa
    
    // Đồng bộ tức thì lên Firebase để App chuyển trạng thái nút sang MỞ
    Firebase.setString(fbdoSend, "/SmartHome/CuaChinh/TheCuoi", uidStr);
    Firebase.setInt(fbdoSend, "/SmartHome/CuaChinh/Servo", 90); 
    
    rfid.PICC_HaltA();
  }

  // TỰ ĐỘNG ĐÓNG CỬA SAU VÀ CHỐT CHẶN KHÔNG LẶP LẠI
  if (isDoorOpenPhysically && (currentMillis - doorOpenMillis >= 5000)) {
    servoDoor.write(0);
    isDoorOpenPhysically = false; // Chuyển trạng thái thực tế về Đóng
    
    // Cập nhật Firebase về 0 ngay để giải phóng bộ nhớ đệm luồng Stream, chặn đứng vòng lặp đơ Servo
    Firebase.setInt(fbdoSend, "/SmartHome/CuaChinh/Servo", 0);
    Serial.println("=> Tu dong dong cua chu dong sau 5s hoan tat.");
  }

  // 2. ĐỌC VÀ CẬP NHẬT DỮ LIỆU CẢM BIẾN LÊN FIREBASE (MỖI 2.5 GIÂY)
  if (currentMillis - lastSensorTime >= 2500) {
    lastSensorTime = currentMillis;
    
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int gasVal = analogRead(GAS_PIN);
    int fireVal = (digitalRead(FIRE_PIN) == LOW) ? 1 : 0; 
    int rainVal = (digitalRead(RAIN_PIN) == LOW) ? 1 : 0; 
    int wcVal = (digitalRead(IR_WC_PIN) == LOW) ? 1 : 0;  

    if (!isnan(t)) {
      Firebase.setFloat(fbdoSend, "/SmartHome/PhongKhach/NhietDo", t);
      Firebase.setFloat(fbdoSend, "/SmartHome/PhongKhach/DoAm", h);
    }
    Firebase.setInt(fbdoSend, "/SmartHome/NhaBep/GasValue", gasVal);
    Firebase.setInt(fbdoSend, "/SmartHome/NhaBep/CoLua", fireVal);
    Firebase.setInt(fbdoSend, "/SmartHome/BanCong/CamBienMua", rainVal);
    Firebase.setInt(fbdoSend, "/SmartHome/NhaVeSinh/CoNguoi", wcVal);
  }

  // 3. XỬ LÝ TOÀN BỘ CÁC KỊCH BẢN TỰ ĐỘNG
  if (currentMillis - lastAutoCheckTime >= 1000) {
    lastAutoCheckTime = currentMillis;
    
    int fireVal = (digitalRead(FIRE_PIN) == LOW) ? 1 : 0;
    int gasVal = analogRead(GAS_PIN);
    int personFrontDoor = (digitalRead(IR_DOOR_PIN) == LOW) ? 1 : 0; 
    int personInWC = (digitalRead(IR_WC_PIN) == LOW) ? 1 : 0; 

    bool hasEmergency = false;

    // --- LOGIC NHÀ VỆ SINH ĐỘC LẬP CHẠY CHUẨN ---
    if (personInWC == 1) {
      digitalWrite(LED_WC, HIGH); 
      digitalWrite(FAN_WC, HIGH); 
    } else {
      digitalWrite(LED_WC, LOW);  
      digitalWrite(FAN_WC, LOW);  
    }

    // --- LOGIC AN NINH: PHÁT HIỆN LỬA HOẶC RÒ RỈ KHÍ GAS ---
    if (fireVal == 1 || gasVal > 400) {
      hasEmergency = true;
      digitalWrite(BUZZER_PIN, HIGH);
      digitalWrite(LED_ALARM, HIGH);
      
      if (gasVal > 400) {
        digitalWrite(FAN_BEP, HIGH); 
        Firebase.setInt(fbdoSend, "/SmartHome/NhaBep/QuatHut", 1);
      }
    } 
    // --- LOGIC AN NINH: CÓ NGƯỜI TRƯỚC NHÀ TRONG VÒNG 5 GIÂY ---
    else if (canhBaoPIR == 1 && personFrontDoor == 1) {
      if (!isWaitingForAlarm) {
        personDetectedMillis = currentMillis; 
        isWaitingForAlarm = true;
      }
      
      if (isWaitingForAlarm && (currentMillis - personDetectedMillis >= 5000)) {
        digitalWrite(BUZZER_PIN, HIGH);
        digitalWrite(LED_ALARM, HIGH);
        hasEmergency = true;
        Firebase.setInt(fbdoSend, "/SmartHome/CuaChinh/Coi", 1); 
      }
    } 
    // --- KHÔNG CÓ SỰ CỐ NGUY HIỂM -> GIẢI PHÓNG HỆ THỐNG CÒI ---
    else {
      isWaitingForAlarm = false; 
      
      if (!hasEmergency && appCoiState == 0) { 
         digitalWrite(BUZZER_PIN, LOW);
         digitalWrite(LED_ALARM, LOW);
      }
    }

    // --- LOGIC TỰ ĐỘNG PHƠI ĐỒ BAN CÔNG ---
    if (autoPhoi == 1) {
      if (digitalRead(RAIN_PIN) == LOW) {
        servoCloth.write(0); 
      } else {
        servoCloth.write(90); 
      }
    }
  }
}