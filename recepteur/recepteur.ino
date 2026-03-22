/*
 * ============================================================
 *  RÉCEPTEUR - SE CONNECTE À "4G de mike"
 *  Reçoit le son en UDP et le joue sur le MAX98357A
 *  VERSION 16-BIT PURE
 * ============================================================
 * 
 *  CÂBLAGE AMPLI AUDIO (MAX98357A) :
 *    VIN  →  5V ou 3.3V
 *    GND  →  GND
 *    LRC  →  GPIO33  (L'équivalent de WS)
 *    BCLK →  GPIO32  (L'équivalent de SCK)
 *    DIN  →  GPIO25  (Sortie I2S des données de l'ESP32 vers l'Ampli)
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "driver/i2s.h"

const char* ssid = "4G de mike";
const char* password = "AZERTY12345678";

WiFiUDP udp;
const int port = 1234;

#define I2S_SCK_PIN   32 // BCLK
#define I2S_WS_PIN    33 // LRC
#define I2S_DOUT_PIN  25 // DIN
#define I2S_RATE      16000

#define UDP_BUFFER_SIZE 1024 

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid, password);
  
  Serial.println("\n============ RÉCEPTEUR ============");
  Serial.print("Connexion au reseau '4G de mike'");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnecté parfaitement !");
  Serial.print("Mon IP locale est : ");
  Serial.println(WiFi.localIP());
  
  udp.begin(port);

  // Config I2S PUREMENT en 16-BIT
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = I2S_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT, 
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false,
    .tx_desc_auto_clear   = true, 
    .fixed_mclk           = 0
  };
  
  i2s_pin_config_t i2s_pins = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num  = I2S_PIN_NO_CHANGE 
  };
  
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_pins);
  Serial.println("Ampli MAX98357A démarré. J'attends la voix !");
}

void loop() {
  uint8_t buffer[UDP_BUFFER_SIZE];
  
  int packetSize = udp.parsePacket();
  
  if (packetSize) {
    int len = udp.read(buffer, UDP_BUFFER_SIZE);
    
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, buffer, len, &bytesWritten, portMAX_DELAY);
  }
}
