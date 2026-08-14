/*
 * Voice AI Assistant — ESP32-S3-Touch-AMOLED-1.75
 *
 * TEST VERSION
 *
 * Purpose:
 *   Make the Waveshare board behave as closely as possible
 *   to the original working ESP32 firmware.
 *
 * Important changes:
 *   - No ESP32-side voice/silence detection
 *   - Send ALL microphone audio, including silence
 *   - 1024 x 32-bit stereo I2S buffer
 *   - 512 samples of 16-bit mono audio per WebSocket packet
 *   - ~32 ms audio packets at 16 kHz
 *   - No 3-second audio timeout
 *   - Python/backend owns speech detection and activation state
 *
 * Backend should send exactly one:
 *
 *     END_AUDIO
 *
 * after TTS playback has finished.
 */

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "driver/i2s.h"
#include "es8311.h"
#include "esp_check.h"


// ============================================================
// WIFI
// ============================================================

const char* ssid     = "TEKAHOME";
const char* password = "connectivity";


// ============================================================
// SERVER
// ============================================================

WebSocketsClient webSocket;

const char* host = "192.168.8.100";
const int port = 8000;
//For Cloud (Azure)
//const char* host = "voiceai-bkdtd6bbc6cgdqch.germanywestcentral-01.azurewebsites.net";
//const int   port = 443;

// ============================================================
// DISPLAY
// ============================================================

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS,
    LCD_SCLK,
    LCD_SDIO0,
    LCD_SDIO1,
    LCD_SDIO2,
    LCD_SDIO3
);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus,
    LCD_RESET,
    0,
    LCD_WIDTH,
    LCD_HEIGHT,
    6,
    0,
    0,
    0
);

int centerX;
int centerY;


// ============================================================
// AUDIO PINS
// ============================================================

#define I2S_MCLK_PIN   42
#define I2S_SPK_MCLK   16

#define I2S_BCLK_PIN   9
#define I2S_WS_PIN    45

#define I2S_MIC_PIN   10
#define I2S_SPK_PIN    8

#define PA_ENABLE      46

#define ES7210_ADDR   0x40


// ============================================================
// STATE
// ============================================================

bool isPlaying   = false;
bool isThinking  = false;
bool i2s_installed = false;

unsigned long lastDebugPrint = 0;
unsigned long chunksSent = 0;

int peakMax = 0;


// ============================================================
// AUDIO BUFFERS
//
// ES7210:
//   32-bit stereo I2S
//
// We read:
//   1024 x 32-bit values
//
// That's:
//   4096 bytes
//
// Stereo frames:
//   4096 / 8 = 512 frames
//
// We convert to:
//   512 x 16-bit mono
//
// At 16 kHz:
//   512 / 16000 = 32 ms
// ============================================================

#define SAMPLE_BUFFER_SIZE 1024

int32_t rawBuf[SAMPLE_BUFFER_SIZE];

int16_t sampleBuffer[SAMPLE_BUFFER_SIZE / 2];


// ============================================================
// DISPLAY
// ============================================================

void drawStateScreen(
    uint16_t ringColor,
    uint16_t fillColor,
    const char* label,
    uint16_t labelColor)
{
    gfx->fillScreen(RGB565_BLACK);

    for (int r = 100; r <= 104; r++)
        gfx->drawCircle(
            centerX,
            centerY,
            r,
            ringColor
        );

    gfx->fillCircle(
        centerX,
        centerY,
        90,
        fillColor
    );

    gfx->setTextColor(labelColor);
    gfx->setTextSize(2, 2, 0);

    int labelX = centerX - (strlen(label) * 6);

    gfx->setCursor(
        labelX,
        centerY + 120
    );

    gfx->println(label);
}


void setScreen_Sleep()
{
    isThinking = false;

    drawStateScreen(
        gfx->color565(0,120,255),
        gfx->color565(0,40,120),
        "Sleeping",
        gfx->color565(100,180,255)
    );

    gfx->fillCircle(
        centerX,
        centerY,
        30,
        gfx->color565(0,120,255)
    );

    gfx->fillCircle(
        centerX + 12,
        centerY - 8,
        22,
        gfx->color565(0,40,120)
    );
}


void setScreen_Listen()
{
    isThinking = false;

    drawStateScreen(
        gfx->color565(0,220,80),
        gfx->color565(0,70,30),
        "Listening",
        gfx->color565(0,220,80)
    );

    gfx->fillRoundRect(
        centerX - 10,
        centerY - 30,
        20,
        36,
        8,
        gfx->color565(0,220,80)
    );

    gfx->drawFastVLine(
        centerX,
        centerY + 30,
        12,
        gfx->color565(0,220,80)
    );

    gfx->drawFastHLine(
        centerX - 10,
        centerY + 42,
        20,
        gfx->color565(0,220,80)
    );
}


void setScreen_Speaking()
{
    isThinking = false;

    drawStateScreen(
        gfx->color565(160,60,255),
        gfx->color565(50,0,100),
        "Speaking",
        gfx->color565(200,140,255)
    );

    int barX = centerX - 30;

    int heights[] = {
        16,
        28,
        40,
        28,
        16
    };

    for (int i = 0; i < 5; i++)
    {
        int h = heights[i];

        gfx->fillRoundRect(
            barX + i * 16,
            centerY - h / 2,
            10,
            h,
            3,
            gfx->color565(200,140,255)
        );
    }
}


void setScreen_Error()
{
    isThinking = false;

    drawStateScreen(
        gfx->color565(255,40,40),
        gfx->color565(100,0,0),
        "Error",
        gfx->color565(255,120,120)
    );

    gfx->drawLine(
        centerX - 20,
        centerY - 20,
        centerX + 20,
        centerY + 20,
        gfx->color565(255,80,80)
    );

    gfx->drawLine(
        centerX + 20,
        centerY - 20,
        centerX - 20,
        centerY + 20,
        gfx->color565(255,80,80)
    );
}


void setScreen_Thinking()
{
    isThinking = true;

    drawStateScreen(
        gfx->color565(255,200,0),
        gfx->color565(80,55,0),
        "Thinking",
        gfx->color565(255,200,0)
    );
}


void animateThinking()
{
    static unsigned long lastFrame = 0;
    static int frame = 0;

    if (millis() - lastFrame < 450)
        return;

    lastFrame = millis();

    gfx->fillRect(
        centerX - 40,
        centerY - 16,
        80,
        32,
        gfx->color565(80,55,0)
    );

    gfx->setTextColor(
        gfx->color565(255,200,0)
    );

    gfx->setTextSize(3, 3, 0);

    String dots = "";

    for (int i = 0; i <= frame; i++)
        dots += ".";

    gfx->setCursor(
        centerX - (dots.length() * 9),
        centerY - 14
    );

    gfx->println(dots);

    frame = (frame + 1) % 3;
}


// ============================================================
// I2C
// ============================================================

bool writeI2C(
    TwoWire &wire,
    uint8_t addr,
    uint8_t reg,
    uint8_t val)
{
    wire.beginTransmission(addr);

    wire.write(reg);
    wire.write(val);

    uint8_t err = wire.endTransmission();

    if (err != 0)
    {
        Serial.printf(
            "I2C error addr=0x%02X reg=0x%02X err=%d\n",
            addr,
            reg,
            err
        );

        return false;
    }

    return true;
}


// ============================================================
// ES8311 SPEAKER
// ============================================================

esp_err_t es8311_codec_init()
{
    es8311_handle_t es_handle =
        es8311_create(
            0,
            ES8311_ADDRRES_0
        );

    if (!es_handle)
        return ESP_FAIL;


    const es8311_clock_config_t es_clk =
    {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = 16000 * 256,
        .sample_frequency = 16000
    };


    ESP_RETURN_ON_ERROR(
        es8311_init(
            es_handle,
            &es_clk,
            ES8311_RESOLUTION_16,
            ES8311_RESOLUTION_16
        ),
        TAG,
        "es8311_init"
    );


    es8311_sample_frequency_config(
        es_handle,
        es_clk.mclk_frequency,
        es_clk.sample_frequency
    );


    es8311_microphone_config(
        es_handle,
        false
    );


    es8311_voice_volume_set(
        es_handle,
        55,
        NULL
    );


    return ESP_OK;
}


// ============================================================
// ES7210 MICROPHONE
// ============================================================

void init_ES7210()
{
    Serial.println(
        "Initializing ES7210..."
    );


    // Software reset

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x00,
        0xFF
    );

    delay(20);


    writeI2C(
        Wire,
        ES7210_ADDR,
        0x00,
        0x32
    );

    delay(20);


    // Initialization timing

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x09,
        0x30
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x0A,
        0x30
    );


    // HPF

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x23,
        0x2A
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x22,
        0x0A
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x21,
        0x2A
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x20,
        0x0A
    );


    // I2S format

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x11,
        0x60
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x12,
        0x00
    );


    // Analog power

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x40,
        0xC3
    );


    // MIC bias

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x41,
        0x70
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x42,
        0x70
    );


    // MIC gain

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x43,
        0x0E | 0x10
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x44,
        0x0E | 0x10
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x45,
        0x0E | 0x10
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x46,
        0x0E | 0x10
    );


    // Power MIC1-4

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x47,
        0x08
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x48,
        0x08
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x49,
        0x08
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x4A,
        0x08
    );


    // Clock

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x07,
        0x20
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x02,
        0xC1
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x04,
        0x01
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x05,
        0x00
    );


    // Power down DLL

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x06,
        0x04
    );


    // Enable device

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x00,
        0x71
    );

    delay(10);


    writeI2C(
        Wire,
        ES7210_ADDR,
        0x00,
        0x41
    );

    delay(20);


    // IMPORTANT:
    // ADC/PGA power AFTER device enable

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x4B,
        0x0F
    );

    writeI2C(
        Wire,
        ES7210_ADDR,
        0x4C,
        0x0F
    );

    delay(100);


    Serial.println(
        "ES7210 initialization complete"
    );
}


// ============================================================
// I2S MICROPHONE
// ============================================================

void i2s_init_mic()
{
    Serial.println(
        "Starting I2S microphone..."
    );


    if (i2s_installed)
    {
        i2s_driver_uninstall(
            I2S_NUM_0
        );

        i2s_installed = false;
    }


    i2s_config_t config =
    {
        .mode =
            (i2s_mode_t)(
                I2S_MODE_MASTER |
                I2S_MODE_RX
            ),

        .sample_rate = 16000,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_32BIT,

        .channel_format =
            I2S_CHANNEL_FMT_RIGHT_LEFT,

        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = 4,

        .dma_buf_len = 256,

        .use_apll = true,

        .tx_desc_auto_clear = false,

        .fixed_mclk = 16000 * 256
    };


    i2s_pin_config_t pins =
    {
        .mck_io_num = I2S_MCLK_PIN,

        .bck_io_num = I2S_BCLK_PIN,

        .ws_io_num = I2S_WS_PIN,

        .data_out_num =
            I2S_PIN_NO_CHANGE,

        .data_in_num =
            I2S_MIC_PIN
    };


    esp_err_t err =
        i2s_driver_install(
            I2S_NUM_0,
            &config,
            0,
            NULL
        );


    Serial.printf(
        "i2s_driver_install: %s\n",
        err == ESP_OK
            ? "OK"
            : esp_err_to_name(err)
    );


    if (err != ESP_OK)
        return;


    i2s_installed = true;


    err = i2s_set_pin(
        I2S_NUM_0,
        &pins
    );


    Serial.printf(
        "i2s_set_pin: %s\n",
        err == ESP_OK
            ? "OK"
            : esp_err_to_name(err)
    );


    Serial.println(
        "MIC READY"
    );
}


// ============================================================
// I2S SPEAKER
// ============================================================

bool initSpeaker()
{
    Serial.println(
        "Starting speaker..."
    );


    if (i2s_installed)
    {
        i2s_driver_uninstall(
            I2S_NUM_0
        );

        i2s_installed = false;
    }


    i2s_config_t config =
    {
        .mode =
            (i2s_mode_t)(
                I2S_MODE_MASTER |
                I2S_MODE_TX
            ),

        .sample_rate = 16000,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_16BIT,

        .channel_format =
            I2S_CHANNEL_FMT_ONLY_LEFT,

        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = 6,

        .dma_buf_len = 512,

        .use_apll = true,

        .tx_desc_auto_clear = true,

        .fixed_mclk = 4096000
    };


    i2s_pin_config_t pins =
    {
        .mck_io_num = MCLKPIN,

        .bck_io_num = BCLKPIN,

        .ws_io_num = WSPIN,

        .data_out_num = DIPIN,

        .data_in_num =
            I2S_PIN_NO_CHANGE
    };


    esp_err_t err;


    err = i2s_driver_install(
        I2S_NUM_0,
        &config,
        0,
        NULL
    );


    if (err != ESP_OK)
    {
        Serial.printf(
            "Speaker I2S install failed: %s\n",
            esp_err_to_name(err)
        );

        return false;
    }


    i2s_installed = true;


    err = i2s_set_pin(
        I2S_NUM_0,
        &pins
    );


    if (err != ESP_OK)
    {
        Serial.printf(
            "Speaker pin configuration failed: %s\n",
            esp_err_to_name(err)
        );

        return false;
    }


    err = es8311_codec_init();


    if (err != ESP_OK)
    {
        Serial.printf(
            "ES8311 init failed: %d\n",
            err
        );

        return false;
    }


    Serial.println(
        "SPEAKER READY"
    );


    return true;
}


// ============================================================
// WEBSOCKET EVENT
// ============================================================

void webSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length)
{
    switch (type)
    {

        // ----------------------------------------------------
        // DISCONNECTED
        // ----------------------------------------------------

        case WStype_DISCONNECTED:

            Serial.println(
                "WebSocket DISCONNECTED"
            );

            setScreen_Error();

            break;


        // ----------------------------------------------------
        // CONNECTED
        // ----------------------------------------------------

        case WStype_CONNECTED:

            Serial.printf(
                "WebSocket CONNECTED to %s:%d\n",
                host,
                port
            );

            setScreen_Sleep();

            break;


        // ----------------------------------------------------
        // TEXT
        // ----------------------------------------------------

        case WStype_TEXT:
        {
            String msg =
                String((char*)payload);

            Serial.printf(
                "WS TEXT: '%s'\n",
                msg.c_str()
            );


            // =================================================
            // END_AUDIO
            //
            // Backend says TTS playback is finished.
            // Switch back to microphone.
            // =================================================

            if (msg == "END_AUDIO")
            {
                Serial.println(
                    "END_AUDIO -> switching to microphone"
                );


                isPlaying = false;
                isThinking = false;


                if (i2s_installed)
                {
                    i2s_driver_uninstall(
                        I2S_NUM_0
                    );

                    i2s_installed = false;
                }


                delay(50);


                i2s_init_mic();


                // Give the microphone a short
                // stabilization period.

                delay(100);


                setScreen_Listen();


                Serial.println(
                    "MIC ACTIVE - streaming audio"
                );
            }


            // =================================================
            // SLEEP
            // =================================================

            else if (msg == "SLEEP")
            {
                Serial.println(
                    "SLEEP received"
                );

                isPlaying = false;
                isThinking = false;

                setScreen_Sleep();
            }


            // =================================================
            // LIGHT
            // =================================================

            else if (msg == "LIGHT_ON")
            {
                digitalWrite(
                    4,
                    HIGH
                );

                Serial.println(
                    "LIGHT ON"
                );
            }


            else if (msg == "LIGHT_OFF")
            {
                digitalWrite(
                    4,
                    LOW
                );

                Serial.println(
                    "LIGHT OFF"
                );
            }


            break;
        }


        // ----------------------------------------------------
        // BINARY AUDIO
        // ----------------------------------------------------

        case WStype_BIN:
        {
            // First audio packet means:
            // switch to speaker.

            if (!isPlaying)
            {
                Serial.println(
                    "First TTS packet -> speaker"
                );


                if (!initSpeaker())
                {
                    Serial.println(
                        "Speaker initialization FAILED"
                    );

                    break;
                }


                isPlaying = true;

                setScreen_Speaking();
            }


            // ------------------------------------------------
            // Volume
            // ------------------------------------------------

            const float VOLUME = 0.6f;


            int16_t* samples =
                (int16_t*)payload;

            int count =
                length / 2;


            for (int i = 0; i < count; i++)
            {
                float s =
                    samples[i] * VOLUME;


                if (s > 32767)
                    s = 32767;


                if (s < -32768)
                    s = -32768;


                samples[i] =
                    (int16_t)s;
            }


            // ------------------------------------------------
            // Write to ES8311
            // ------------------------------------------------

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


        // ----------------------------------------------------
        // ERROR
        // ----------------------------------------------------

        case WStype_ERROR:

            Serial.println(
                "WebSocket ERROR"
            );

            setScreen_Error();

            break;


        case WStype_PING:

            Serial.println(
                "WS PING"
            );

            break;


        case WStype_PONG:

            Serial.println(
                "WS PONG"
            );

            break;


        default:

            break;
    }
}


// ============================================================
// MICROPHONE AUDIO STREAM
//
// IMPORTANT:
//   No VAD here.
//
// Every frame is sent.
//
// This lets Azure receive both:
//   speech
//   silence
//
// and makes this firmware behave much more like
// the original working ESP32 firmware.
// ============================================================

void send_audio_chunk()
{
    if (!i2s_installed)
        return;


    size_t bytes_read = 0;


    i2s_read(
        I2S_NUM_0,
        rawBuf,
        sizeof(rawBuf),
        &bytes_read,
        portMAX_DELAY
    );


    if (bytes_read == 0)
        return;


    // --------------------------------------------------------
    // Stereo 32-bit → mono 16-bit
    //
    // Each stereo frame:
    //
    // LEFT  = 4 bytes
    // RIGHT = 4 bytes
    //
    // Total = 8 bytes
    // --------------------------------------------------------

    int frames =
        bytes_read / 8;


    if (frames <= 0)
        return;


    int peak = 0;


    for (int i = 0; i < frames; i++)
    {
        // LEFT channel
        int32_t raw =
            rawBuf[i * 2];


        // ES7210 audio is in upper 16 bits

        int16_t sample =
            (int16_t)(raw >> 16);


        sampleBuffer[i] =
            sample;


        int value =
            abs((int)sample);


        if (value > peak)
            peak = value;


        if (value > peakMax)
            peakMax = value;
    }


    // --------------------------------------------------------
    // SEND EVERYTHING
    //
    // DO NOT filter silence.
    // --------------------------------------------------------

    if (webSocket.isConnected())
    {
        webSocket.sendBIN(
            (uint8_t*)sampleBuffer,
            frames * 2
        );

        chunksSent++;
    }


    // --------------------------------------------------------
    // Debug approximately every 1 second
    // --------------------------------------------------------

    if (millis() - lastDebugPrint > 1000)
    {
        Serial.printf(
            "MIC: peak=%d max=%d frames=%d bytes=%d "
            "chunks=%lu WS=%s playing=%d\n",
            peak,
            peakMax,
            frames,
            frames * 2,
            chunksSent,
            webSocket.isConnected()
                ? "OK"
                : "NO",
            isPlaying
        );


        peakMax = 0;

        lastDebugPrint =
            millis();
    }
}


// ============================================================
// WIFI
// ============================================================

void connect_wifi()
{
    Serial.printf(
        "Connecting to WiFi: %s\n",
        ssid
    );


    WiFi.begin(
        ssid,
        password
    );


    int attempts = 0;


    while (
        WiFi.status() != WL_CONNECTED
    )
    {
        delay(500);

        Serial.print(".");


        if (++attempts > 40)
        {
            Serial.println(
                "\nWiFi FAILED"
            );

            return;
        }
    }


    Serial.printf(
        "\nWiFi OK IP=%s RSSI=%d dBm\n",
        WiFi.localIP()
            .toString()
            .c_str(),
        WiFi.RSSI()
    );
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    esp_log_level_set(
        "i2s(legacy)",
        ESP_LOG_NONE
    );


    Serial.begin(115200);

    delay(500);


    Serial.println();
    Serial.println(
        "======================================"
    );

    Serial.println(
        " Voice AI - Waveshare TEST VERSION"
    );

    Serial.println(
        "======================================"
    );


    // --------------------------------------------------------
    // Amplifier
    // --------------------------------------------------------

    pinMode(
        PA_ENABLE,
        OUTPUT
    );

    digitalWrite(
        PA_ENABLE,
        HIGH
    );


    Serial.printf(
        "PA_ENABLE GPIO %d -> HIGH\n",
        PA_ENABLE
    );


    // --------------------------------------------------------
    // Light
    // --------------------------------------------------------

    pinMode(
        4,
        OUTPUT
    );

    digitalWrite(
        4,
        LOW
    );


    // --------------------------------------------------------
    // I2C
    // --------------------------------------------------------

    Wire.begin(
        IIC_SDA,
        IIC_SCL
    );


    Serial.printf(
        "I2C SDA=%d SCL=%d\n",
        IIC_SDA,
        IIC_SCL
    );


    // --------------------------------------------------------
    // Display
    // --------------------------------------------------------

    if (!gfx->begin())
    {
        Serial.println(
            "Display initialization FAILED"
        );
    }
    else
    {
        Serial.println(
            "Display OK"
        );
    }


    gfx->fillScreen(
        RGB565_BLACK
    );


    gfx->setBrightness(
        180
    );


    centerX =
        gfx->width() / 2;

    centerY =
        gfx->height() / 2;


    setScreen_Sleep();


    // --------------------------------------------------------
    // Initialize speaker codec
    //
    // This also initializes ES8311.
    // It temporarily installs I2S TX.
    // We will switch to microphone below.
    // --------------------------------------------------------

    if (!initSpeaker())
    {
        Serial.println(
            "Speaker initialization FAILED"
        );
    }


    // --------------------------------------------------------
    // ES7210 microphone ADC
    // --------------------------------------------------------

    init_ES7210();


    // --------------------------------------------------------
    // WiFi
    // --------------------------------------------------------

    connect_wifi();


    // --------------------------------------------------------
    // Switch I2S to microphone
    // --------------------------------------------------------

    i2s_init_mic();


    // --------------------------------------------------------
    // Give microphone some time to stabilize.
    // --------------------------------------------------------

    delay(200);


    // --------------------------------------------------------
    // WebSocket
    // --------------------------------------------------------

    Serial.printf(
        "Connecting WebSocket to %s:%d\n",
        host,
        port
    );


    webSocket.begin(
        host,
        port,
        "/ws/audio"
    );
    //webSocket.beginSSL(host, port, "/ws/audio"); // for cloud

    webSocket.onEvent(
        webSocketEvent
    );


    webSocket.setReconnectInterval(
        3000
    );


    Serial.println();
    Serial.println(
        "======================================"
    );

    Serial.println(
        " READY"
    );

    Serial.println(
        " Say: Hello Assistant"
    );

    Serial.println(
        "======================================"
    );

    Serial.println();
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
    // WebSocket must run frequently.

    webSocket.loop();


    // --------------------------------------------------------
    // Microphone is always streamed when we're not playing.
    //
    // No ESP32-side VAD.
    // No silence timeout.
    // No activation check.
    // --------------------------------------------------------

    if (!isPlaying)
    {
        send_audio_chunk();
    }


    // --------------------------------------------------------
    // Thinking animation
    // --------------------------------------------------------

    if (isThinking)
    {
        animateThinking();
    }


    delay(1);
}
