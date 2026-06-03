#include <WiFi.h>
#include <WebSocketsClient.h>
#include "driver/i2s.h"

// ================= WIFI =================
const char* ssid = "TEKAHOME";
const char* password = "connectivity";

// ================= SERVER =================
WebSocketsClient webSocket;
const char* host = "192.168.8.103";  // YOUR PC IP
const int port = 8000;

// ================= AUDIO =================
#define I2S_WS 18
#define I2S_SD 16
#define I2S_SCK 17
#define I2S_DOUT 15
#define STOP_BUTTON 4

#define SAMPLE_RATE 16000
#define SAMPLE_BUFFER_SIZE 1024

int32_t i2sBuffer[SAMPLE_BUFFER_SIZE];
int16_t sampleBuffer[SAMPLE_BUFFER_SIZE];
bool isPlaying = false;

// ================= I2S =================
void i2s_init_mic() {

  i2s_driver_uninstall(I2S_NUM_0);

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  Serial.println("MIC READY ✅");
}

void i2s_init_speaker() {

  i2s_driver_uninstall(I2S_NUM_0);

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = true   // ✅ IMPORTANT
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  Serial.println("SPEAKER READY ✅");
}

// ================= WEBSOCKET EVENTS =================
unsigned long lastAudioReceived = 0;

void webSocketEvent(
    WStype_t type,
    uint8_t *payload,
    size_t length)
{
    switch(type)
    {
        case WStype_DISCONNECTED:
            Serial.println("❌ WebSocket disconnected");
            break;

        case WStype_CONNECTED:
            Serial.println("✅ WebSocket connected");
            break;

        case WStype_TEXT:
        {
            String msg = String((char*)payload);

            Serial.printf("📨 TEXT: %s\n", msg.c_str());

            if(msg == "END_AUDIO")
            {
                Serial.println("🎤 Back to MIC");

                i2s_init_mic();
                isPlaying = false;
            }

            break;
        }

        case WStype_BIN:
        {
            if(!isPlaying)
            {
                i2s_init_speaker();
                isPlaying = true;
                Serial.println("🔊 Playing response...");
            }

            lastAudioReceived = millis();

            const float VOLUME = 0.5f;

            int16_t *samples = (int16_t*)payload;
            int sample_count = length / 2;

            for(int i = 0; i < sample_count; i++)
            {
                float scaled = samples[i] * VOLUME;

                if(scaled > 32767)
                    scaled = 32767;

                if(scaled < -32768)
                    scaled = -32768;

                samples[i] = (int16_t)scaled;
            }

            size_t written = 0;

            i2s_write(
                I2S_NUM_0,
                payload,
                length,
                &written,
                portMAX_DELAY
            );

            break;
        }

        case WStype_ERROR:
            Serial.println("⚠️ WebSocket error");
            break;

        default:
            break;
    }
}

// ================= AUDIO STREAM =================
void send_audio_chunk()
{
    size_t bytes_read = 0;

    i2s_read(
        I2S_NUM_0,
        i2sBuffer,
        sizeof(i2sBuffer),
        &bytes_read,
        portMAX_DELAY
    );

    if(bytes_read == 0)
        return;

    int samples = bytes_read / 4;

    int peak = 0;

    for(int i = 0; i < samples; i++)
    {
        int32_t raw = i2sBuffer[i] >> 16;

        raw *= 2;

        if(raw > 32767) raw = 32767;
        if(raw < -32768) raw = -32768;

        sampleBuffer[i] = (int16_t)raw;

        int absVal = abs(sampleBuffer[i]);

        if(absVal > peak)
            peak = absVal;
    }

    static unsigned long lastPrint = 0;

    if(millis() - lastPrint > 1000)
    {
        Serial.printf("MIC PEAK=%d\n", peak);
        lastPrint = millis();
    }

    if(peak < 500)
        return;

    if(webSocket.isConnected())
    {
        webSocket.sendBIN(
            (uint8_t*)sampleBuffer,
            samples * 2
        );
    }
}

// ================= WIFI =================
void connect_wifi() {
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  connect_wifi();

  i2s_init_mic();   // ✅ ONLY MIC

  webSocket.begin(host, port, "/ws/audio");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  pinMode(STOP_BUTTON, INPUT_PULLUP);
}

// ================= LOOP =================
unsigned long lastSend = 0;

void loop() {

  webSocket.loop();

  if (!isPlaying) {
      send_audio_chunk();
  }

  delay(1);  // ✅ keep system responsive
}