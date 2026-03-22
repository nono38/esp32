/*
 * ============================================================
 *  ÉMETTEUR - POINT D'ACCÈS WIFI "4G de mike"
 *  Lit le micro INMP441 et diffuse l'audio en UDP
 *  VERSION 16-BIT PURE
 * ============================================================
 * 
 *  RAPPEL DU CÂBLAGE MICRO NUMÉRIQUE (INMP441) :
 *    VDD  →  3.3V
 *    GND  →  GND
 *    L/R  →  GND
 *    SCK  →  GPIO32
 *    WS   →  GPIO33
 *    SD   →  GPIO35
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "driver/i2s.h"

// Configuration du réseau "maison"
const char* ssid = "4G de mike";
const char* password = "AZERTY12345678"; 

WiFiUDP udp;
const char* host = "192.168.4.255"; 
const int port = 1234;

// Broches du Micro
#define I2S_SCK_PIN   32
#define I2S_WS_PIN    33
#define I2S_SD_PIN    35
#define I2S_RATE      16000

#define CHUNK_SAMPLES 512

void setup() {
  Serial.begin(115200);
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.println("\n============ ÉMETTEUR ============");
  Serial.println("Réseau '4G de mike' créé avec succès !");
  Serial.print("Mon IP AP est : ");
  Serial.println(WiFi.softAPIP());
  
  // Config I2S PUREMENT en 16-BIT
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = I2S_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = CHUNK_SAMPLES,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  
  i2s_pin_config_t i2s_pins = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, 
    .data_in_num  = I2S_SD_PIN
  };
  
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_pins);
  Serial.println("Microphone I2S démarré. Diffusion en cours...");
}

void loop() {
  uint8_t udp_buffer[CHUNK_SAMPLES * 2]; // Lecture directe de 16 bits (2 octets) par échantillon
  size_t bytesRead = 0;
  
  // Lecture du micro directement dans le buffer
  i2s_read(I2S_NUM_0, udp_buffer, sizeof(udp_buffer), &bytesRead, portMAX_DELAY);
  
  // Envoyer au récepteur via UDP
  udp.beginPacket(host, port);
  udp.write(udp_buffer, bytesRead);
  udp.endPacket(); 
}
