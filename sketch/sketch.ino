/*
 * NORVI XN  -  XY-MD02 Modbus RTU Temperature / Humidity display
 *
 * Sensor : XY-MD02 (SHT20), RS485 Modbus RTU, slave ID 1, 9600 8N1
 *          FC 0x04, input reg 0x0001 = temperature (signed), 0x0002 = humidity
 * Display: ST7789 240x320 on SPI (portrait)
 * RS485  : Serial1  RX=16  TX=15 (auto-direction transceiver)
 */
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <ModbusMaster.h>

// ---------------- Display driver ----------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc   = 46;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 45;
      cfg.pin_rst          = 47;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;
LGFX_Sprite card(&tft);
ModbusMaster node;

// ---------------- Configuration ----------------
#define RS485_RX_PIN     16
#define RS485_TX_PIN     15
#define MODBUS_BAUD      9600
#define MD02_SLAVE_ID    1
#define POLL_INTERVAL_MS 2000

// ---------------- Colours ----------------
#define C_BG       0x0000
#define C_GREEN    0x2E8B   // NORVI green
#define C_CARD     0x18E3
#define C_LABEL    0xA514
#define C_TEMP     0xFD20   // orange
#define C_HUM      0x5D1F   // light blue
#define C_ERR      0xF800

uint32_t okCount = 0, errCount = 0;
float    divisor = 10.0f;   // datasheet scale; auto-corrected to 100 if sensor reports x100
bool     scaleLocked = false;

// ---------------- UI helpers ----------------
void drawHeader() {
  tft.fillRect(0, 0, 240, 44, C_GREEN);
  tft.setTextColor(TFT_WHITE, C_GREEN);
  tft.setTextDatum(middle_left);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.drawString("NORVI XN", 10, 22);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_right);
  tft.drawString("XY-MD02", 230, 22);
}

void drawValueCard(int y, const char* label, const char* value, const char* unit, uint16_t colour) {
  card.fillSprite(C_BG);
  card.fillRoundRect(0, 0, 220, 100, 10, C_CARD);
  card.fillRoundRect(0, 0, 6, 100, 3, colour);

  card.setTextColor(C_LABEL, C_CARD);
  card.setTextDatum(top_left);
  card.setFont(&fonts::Font2);
  card.drawString(label, 18, 10);

  card.setTextColor(colour, C_CARD);
  card.setFont(&fonts::FreeSansBold24pt7b);
  card.setTextDatum(bottom_left);
  card.drawString(value, 18, 88);
  int w = card.textWidth(value);

  card.setFont(&fonts::FreeSans12pt7b);
  card.drawString(unit, 24 + w, 84);

  card.pushSprite(10, y);
}

void drawStatus(bool ok, uint8_t code) {
  tft.fillRect(0, 286, 240, 34, C_BG);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_left);
  char buf[48];
  if (ok) {
    tft.fillCircle(16, 303, 5, C_GREEN);
    tft.setTextColor(TFT_WHITE, C_BG);
    snprintf(buf, sizeof(buf), "Modbus OK  ID %d  %d bd", MD02_SLAVE_ID, MODBUS_BAUD);
  } else {
    tft.fillCircle(16, 303, 5, C_ERR);
    tft.setTextColor(C_ERR, C_BG);
    const char* why = (code == ModbusMaster::ku8MBResponseTimedOut) ? "No response" :
                      (code == ModbusMaster::ku8MBInvalidCRC)       ? "CRC error"   :
                      (code == ModbusMaster::ku8MBInvalidSlaveID)   ? "Wrong ID"    : "Comm error";
    snprintf(buf, sizeof(buf), "%s (0x%02X)", why, code);
  }
  tft.drawString(buf, 28, 303);

  tft.setTextColor(C_LABEL, C_BG);
  tft.setTextDatum(middle_right);
  snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)okCount, (unsigned long)errCount);
  tft.drawString(buf, 232, 303);
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  Serial1.begin(MODBUS_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  node.begin(MD02_SLAVE_ID, Serial1);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  card.setColorDepth(16);
  card.createSprite(220, 100);

  drawHeader();
  drawValueCard(60,  "TEMPERATURE", "--.-", "C",  C_TEMP);
  drawValueCard(175, "HUMIDITY",    "--.-", "%RH", C_HUM);
  drawStatus(false, ModbusMaster::ku8MBResponseTimedOut);

  Serial.println("NORVI XN - XY-MD02 reader started");
}

// ---------------- Loop ----------------
void loop() {
  static uint32_t last = 0;
  if (millis() - last < POLL_INTERVAL_MS && last != 0) return;
  last = millis();

  // FC 0x04, start 0x0001, qty 2 -> temperature, humidity
  uint8_t result = node.readInputRegisters(0x0001, 2);

  if (result == node.ku8MBSuccess) {
    int16_t  tempRaw = (int16_t)node.getResponseBuffer(0);   // signed per datasheet
    uint16_t humRaw  = node.getResponseBuffer(1);

    // Datasheet scale is x10 (546 -> 54.6 %RH). Some MD02 firmwares report x100.
    // Humidity can't exceed 100.0 %RH, so a raw value > 1000 means x100 scaling.
    if (!scaleLocked) {
      divisor = (humRaw > 1000) ? 100.0f : 10.0f;
      scaleLocked = true;
      Serial.printf("Scale detected: /%.0f\n", divisor);
    }

    float tempC = tempRaw / divisor;
    float hum   = humRaw  / divisor;
    okCount++;

    char t[12], h[12];
    snprintf(t, sizeof(t), "%.1f", tempC);
    snprintf(h, sizeof(h), "%.1f", hum);
    drawValueCard(60,  "TEMPERATURE", t, "C",   C_TEMP);
    drawValueCard(175, "HUMIDITY",    h, "%RH", C_HUM);
    drawStatus(true, result);

    Serial.printf("T=%.1f C  RH=%.1f %%  (raw %d / %u)\n", tempC, hum, tempRaw, humRaw);
  } else {
    errCount++;
    drawStatus(false, result);
    Serial.printf("Modbus read failed, code 0x%02X\n", result);
  }
}
