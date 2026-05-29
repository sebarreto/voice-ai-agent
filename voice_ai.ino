#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include <math.h>

// ================= WIFI =================
const char* ssid = "TEKAHOME";
const char* password = "connectivity";

// Backend endpoint
const char* serverUrl = "http://192.168.8.106:5000/api/assistant";

// ================= AUDIO CONFIG =================
#define I2S_WS 18
#define I2S_SD 16
#define I2S_SCK 17
#define BUTTON_PIN 4
#define I2S_DOUT 15

#define SAMPLE_RATE 16000
#define SAMPLE_BUFFER_SIZE 512

#define RECORD_SECONDS 4
#define MAX_SAMPLES (SAMPLE_RATE * RECORD_SECONDS)

// ================= VAD =================
#define VAD_THRESHOLD 1500

// ================= GLOBALS =================
int32_t i2sBuffer[SAMPLE_BUFFER_SIZE];
int16_t sampleBuffer[SAMPLE_BUFFER_SIZE];
int16_t *record_buffer;
uint8_t *wav_buffer;
int record_index = 0;
bool is_recording = false;
bool ready_to_send = false;

// ================= I2S INIT =================
void i2s_init_mic() {

    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t config = {

        .mode = (i2s_mode_t)(
            I2S_MODE_MASTER |
            I2S_MODE_RX
        ),

        .sample_rate = SAMPLE_RATE,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_32BIT,

        .channel_format =
            I2S_CHANNEL_FMT_ONLY_LEFT,

        .communication_format =
            I2S_COMM_FORMAT_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = 8,

        .dma_buf_len = 256,

        .use_apll = false,

        .tx_desc_auto_clear = false,

        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {

        .bck_io_num = I2S_SCK,

        .ws_io_num = I2S_WS,

        .data_out_num = I2S_DOUT,

        .data_in_num = I2S_SD
    };

    i2s_driver_install(
        I2S_NUM_0,
        &config,
        0,
        NULL
    );

    i2s_set_pin(I2S_NUM_0, &pin_config);

    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.println("I2S MIC READY");
}

void i2s_init_speaker() {

    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t config = {

        .mode = (i2s_mode_t)(
            I2S_MODE_MASTER |
            I2S_MODE_TX
        ),

        .sample_rate = 16000,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_16BIT,

        .channel_format =
          I2S_CHANNEL_FMT_ONLY_LEFT,

        .communication_format =
            I2S_COMM_FORMAT_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = 8,

        //.dma_buf_len = 256,
        .dma_buf_len = 1024,

        .use_apll = false,

        .tx_desc_auto_clear = true,

        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {

        .bck_io_num = I2S_SCK,

        .ws_io_num = I2S_WS,

        .data_out_num = I2S_DOUT,

        .data_in_num = I2S_SD
    };

    i2s_driver_install(
        I2S_NUM_0,
        &config,
        0,
        NULL
    );

    i2s_set_pin(I2S_NUM_0, &pin_config);

    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.println("I2S SPEAKER READY");
}


void play_audio_stream(HTTPClient &http) {

    WiFiClient *stream = http.getStreamPtr();

    i2s_init_speaker();

    // ===== SKIP WAV HEADER =====
    //uint8_t wav_header[44];
    //stream->readBytes(wav_header, 44);

    Serial.println("Playing audio...");

    uint8_t buffer[1024];

    while (stream->available() < 2048) {
        delay(1);
    }

    while (http.connected() || stream->available()) {

        size_t available = stream->available();

        if (available) {

            int len = stream->readBytes(
                buffer,
                min(available, sizeof(buffer))
            );

            if (len <= 0) {
                yield();
                continue;
            }

            // IMPORTANTÍSIMO
            if (len % 2 != 0) {
                len--;
            }

            // evitar microchunks
            if (len < 64) {
                yield();
                continue;
            }

            int16_t *samples = (int16_t*)buffer;

            for (int i = 0; i < len / 2; i++) {

                int32_t scaled =
                //    samples[i] * 0.18f;
                     samples[i] * 0.30f;

                if (scaled > 32767)
                    scaled = 32767;

                if (scaled < -32768)
                    scaled = -32768;

                samples[i] = (int16_t)scaled;
            }

            size_t written;

            i2s_write(
                I2S_NUM_0,
                buffer,
                len,
                &written,
                portMAX_DELAY
            );
        }

        yield();
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    i2s_init_mic();

    Serial.println("Playback finished");
}

// ================= SEND AUDIO =================
void send_audio() {

    Serial.println("Sending audio...");

    HTTPClient http;

    http.setConnectTimeout(15000);
    http.setTimeout(30000);

    http.begin(serverUrl);

    http.addHeader(
        "Content-Type",
        "audio/raw;encoding=signed-integer;bits=16;rate=16000;endian=little"
    );

    int audio_size = record_index * 2;

    int response = http.POST(
        (uint8_t*)record_buffer,
        audio_size
    );

    Serial.print("HTTP Response: ");
    Serial.println(response);

    if (response > 0) {

        play_audio_stream(http);
    }
    else {

        Serial.print("HTTP error: ");
        Serial.println(response);
    }

    http.end();

    record_index = 0;

    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.println("Ready for next recording");
}

// ================= LOOP AUDIO =================
void process_audio() {

    size_t bytes_read;

    i2s_read(
        I2S_NUM_0,
        i2sBuffer,
        sizeof(i2sBuffer),
        &bytes_read,
        portMAX_DELAY
    );

    int samples = bytes_read / 4;

    // ===== CONVERT 32 -> 16 =====
    for (int i = 0; i < samples; i++) {
        sampleBuffer[i] = (int16_t)(i2sBuffer[i] >> 16);
    }

    // ===== REMOVE DC OFFSET =====
    int32_t mean = 0;

    for (int i = 0; i < samples; i++) {
        mean += sampleBuffer[i];
    }

    mean /= samples;

    for (int i = 0; i < samples; i++) {
        sampleBuffer[i] -= mean;
    }

    // ===== RMS =====
    float rms = 0;

    for (int i = 0; i < samples; i++) {
        float s = sampleBuffer[i];
        rms += s * s;
    }

    rms = sqrt(rms / samples);

    Serial.println(rms);

    static int silence_count = 0;

    // ===== VAD START =====
    if (rms > VAD_THRESHOLD && !is_recording) {

        is_recording = true;
        record_index = 0;
        silence_count = 0;

        Serial.println("VOICE START");
    }

    // ===== RECORD =====
    if (is_recording) {

        for (int i = 0; i < samples; i++) {

            if (record_index < MAX_SAMPLES) {

                // gain
                int32_t amplified = sampleBuffer[i] * 8;

                // clipping
                if (amplified > 32767) amplified = 32767;
                if (amplified < -32768) amplified = -32768;

                int16_t processed = amplified;

                // noise gate
                if (abs(processed) < 80) {
                    processed = 0;
                }

                record_buffer[record_index++] = processed;
            }
        }

        // ===== SILENCE =====
        if (rms < 400)
            silence_count++;
        else
            silence_count = 0;

        // ===== STOP =====
        if (silence_count > 50 || record_index >= MAX_SAMPLES) {

            is_recording = false;
            ready_to_send = true;

            Serial.println("VOICE END");
        }
    }
}

// ================= WIFI =================
void connect_wifi() {
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nConnected!");
}

// ================= SETUP =================
void setup() {

    Serial.begin(115200);

    connect_wifi();

    i2s_init_mic();

    Serial.println(psramFound());

    record_buffer = (int16_t*) ps_malloc(
        MAX_SAMPLES * sizeof(int16_t)
    );

    wav_buffer = (uint8_t*) ps_malloc(
        44 + (MAX_SAMPLES * 2)
    );

    if (!record_buffer || !wav_buffer) {

        Serial.println("PSRAM allocation failed!");

        while(true);
    }

    Serial.println("System ready");
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void record_audio() {

    Serial.println("Recording...");

    record_index = 0;

    while (digitalRead(BUTTON_PIN) == LOW) {

        size_t bytes_read;

        i2s_read(
            I2S_NUM_0,
            i2sBuffer,
            sizeof(i2sBuffer),
            &bytes_read,
            portMAX_DELAY
        );

        int samples = bytes_read / 4;

        for (int i = 0; i < samples; i++) {

            int16_t sample =
                (int16_t)(i2sBuffer[i] >> 16);

            // pequeño gain opcional
            int32_t amplified = sample * 2;

            if (amplified > 32767) amplified = 32767;
            if (amplified < -32768) amplified = -32768;

            if (record_index >= MAX_SAMPLES) {
                break;
            }

            if (record_index < MAX_SAMPLES) {
                record_buffer[record_index++] =
                    (int16_t)amplified;
            }
        }
    }

    Serial.println("Recording finished");
}

void loop() {

    if (digitalRead(BUTTON_PIN) == LOW) {

        delay(50);

        if (digitalRead(BUTTON_PIN) == LOW) {

            record_audio();

            if (record_index > 1000) {

                send_audio();
            }

            delay(500);
        }
    }
}

// ================= MAIN LOOP =================
//void loop() {
//
//    process_audio();
//
//    if (ready_to_send) {
//        ready_to_send = false;
//        send_audio();
//    }
//}