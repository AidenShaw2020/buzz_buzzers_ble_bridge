/*
 * ESP32-S3 Buzz -> BLE Gamepad bridge (Arduino IDE)
 * - USB host: esp32beans/ESP32_USB_Host_HID
 * - BLE gamepad: ESP32-BLE-Gamepad
 *
 * Multi-press + pevné mapování dle hexdumpů.
 * Barvy v každé pětici: R, B, O, G, Y
 * Pořadí hráčů (permutace skupin): 1→2, 2→4, 3→1, 4→3
 *   (= skupina P1 jde do BLE P2; P2→P4; P3→P1; P4→P3)
 */

#include <Arduino.h>
#include <BleGamepad.h>

extern "C" {
  #include "esp_log.h"
  #include "esp_event.h"
  #include "driver/gpio.h"
  #include "usb/usb_host.h"
  #include "hid_host.h"
}

// ---------- Volitelný VBUS spínač ----------
#define VBUS_EN_PIN   -1   // nastav GPIO pro VBUS_EN, nebo nech -1 pokud nemáš spínač
#define VBUS_ON_LEVEL  1

static inline void power_vbus_on() {
#if (VBUS_EN_PIN >= 0)
  pinMode(VBUS_EN_PIN, OUTPUT);
  digitalWrite(VBUS_EN_PIN, VBUS_ON_LEVEL);
#endif
}

// ---------- Helpers ----------
static const char *TAG = "buzz";

static void dump_hex(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if ((i % 16) == 0) Serial.print("\n  ");
    Serial.printf("%02X ", p[i]);
  }
  Serial.println();
}

// ---------- BLE gamepad ----------
static BleGamepad bleGamepad("Buzz BLE Bridge", "ESP32-S3", 100);
static volatile uint32_t s_prev_bits = 0; // poslední stav 20 tlačítek (bity 0..19)
static const bool INVERT_BITS = false;    // pokud někdy zjistíš inverzi, přepni na true

// bit 0 -> BLE 1, bit 19 -> BLE 20
static inline uint8_t bitIndexToBleButton(uint8_t bitIndex) {
  return (bitIndex < 20) ? (bitIndex + 1) : 0;
}

// pouze rozdíly pošleme do BLE (press/release)
static void push_bits_to_ble(uint32_t bits)
{
  uint32_t cur  = INVERT_BITS ? ~bits        : bits;
  uint32_t prev = INVERT_BITS ? ~s_prev_bits : s_prev_bits;
  uint32_t changed = cur ^ prev;

  if (!bleGamepad.isConnected()) {
    s_prev_bits = bits;
    return;
  }

  for (uint8_t i = 0; i < 20; ++i) {
    uint32_t mask = (1U << i);
    if (changed & mask) {
      uint8_t b = bitIndexToBleButton(i);
      if (!b) continue;
      if (cur & mask) bleGamepad.press(b);
      else            bleGamepad.release(b);
    }
  }
  s_prev_bits = bits;
}

// ---------- Dekodér 5B reportu -> 20bit bitfield (pořadí R, B, O, G, Y) ----------
static uint32_t decode_bitfield_from_report_5B(const uint8_t *d, size_t n)
{
  if (n < 5) return 0;
  const uint8_t b2 = d[2];
  const uint8_t b3 = d[3];
  const uint8_t b4 = d[4];
  const uint8_t lsn = (uint8_t)(b4 & 0x0F); // nízký nibbl b4

  uint32_t bits = 0;

  // P1 -> bits 0..4: R,B,O,G,Y
  if (b2 & 0x20) bits |= (1U << 0);  // P1 R
  if (b3 & 0x02) bits |= (1U << 1);  // P1 B
  if (b3 & 0x01) bits |= (1U << 2);  // P1 O
  if (b2 & 0x80) bits |= (1U << 3);  // P1 G
  if (b2 & 0x40) bits |= (1U << 4);  // P1 Y

  // P2 -> bits 5..9: R,B,O,G,Y
  if (b3 & 0x80) bits |= (1U << 5);  // P2 R
  if (lsn & 0x08) bits |= (1U << 6); // P2 B
  if (lsn & 0x04) bits |= (1U << 7); // P2 O
  if (lsn & 0x02) bits |= (1U << 8); // P2 G
  if (lsn & 0x01) bits |= (1U << 9); // P2 Y

  // P3 -> bits 10..14: R,B,O,G,Y
  if (b2 & 0x01) bits |= (1U << 10); // P3 R
  if (b2 & 0x10) bits |= (1U << 11); // P3 B
  if (b2 & 0x08) bits |= (1U << 12); // P3 O
  if (b2 & 0x04) bits |= (1U << 13); // P3 G
  if (b2 & 0x02) bits |= (1U << 14); // P3 Y

  // P4 -> bits 15..19: R,B,O,G,Y
  if (b3 & 0x04) bits |= (1U << 15); // P4 R
  if (b3 & 0x40) bits |= (1U << 16); // P4 B
  if (b3 & 0x20) bits |= (1U << 17); // P4 O
  if (b3 & 0x10) bits |= (1U << 18); // P4 G
  if (b3 & 0x08) bits |= (1U << 19); // P4 Y

  return bits;
}

// ---------- Permutace hráčů: 1→2, 2→4, 3→1, 4→3 ----------
static uint32_t remap_player_groups(uint32_t bits_in)
{
  // 4 skupiny po 5 bitech (0..4, 5..9, 10..14, 15..19)
  // map[cur] = dst (0-based)
  static const uint8_t map_dst_by_cur[4] = { 1, 3, 0, 2 }; // 1→2, 2→4, 3→1, 4→3
  uint32_t out = 0;
  for (int cur = 0; cur < 4; ++cur) {
    uint32_t grp = (bits_in >> (cur * 5)) & 0x1F;
    int dst = map_dst_by_cur[cur];
    out |= (grp << (dst * 5));
  }
  return out;
}

// ---------- HID callbacks ----------
static void handle_buzz_report(const uint8_t *data, size_t len)
{
  // hexdump (vypni po odladění klidně)
  Serial.printf("IN report %u B:", (unsigned)len);
  dump_hex(data, len);

  uint32_t bits = decode_bitfield_from_report_5B(data, len);
  bits = remap_player_groups(bits);          // <<< přeuspořádání hráčů
  push_bits_to_ble(bits);
}

extern "C" void hid_host_interface_callback(hid_host_device_handle_t dev,
                                            const hid_host_interface_event_t event,
                                            void *arg)
{
  (void)arg;
  uint8_t  buf[64] = {0};
  size_t   n = 0;

  hid_host_dev_params_t params;
  if (hid_host_device_get_params(dev, &params) != ESP_OK) return;

  switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
      if (hid_host_device_get_raw_input_report_data(dev, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
        handle_buzz_report(buf, n);
      }
      break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "HID DISCONNECTED (proto=%d)", params.proto);
      hid_host_device_close(dev);
      // Pusť všechna tlačítka
      if (bleGamepad.isConnected()) {
        for (int i = 1; i <= 20; ++i) bleGamepad.release(i);
      }
      s_prev_bits = 0;
      break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
      ESP_LOGW(TAG, "HID TRANSFER_ERROR (proto=%d)", params.proto);
      break;

    default:
      ESP_LOGW(TAG, "HID unhandled if event=%d", (int)event);
      break;
  }
}

extern "C" void hid_host_device_callback(hid_host_device_handle_t dev,
                                         const hid_host_driver_event_t event,
                                         void *arg);

static void hid_host_device_event(hid_host_device_handle_t dev,
                                  const hid_host_driver_event_t event, void *arg)
{
  (void)arg;
  hid_host_dev_params_t params;
  if (hid_host_device_get_params(dev, &params) != ESP_OK) return;

  switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED: {
      ESP_LOGI(TAG, "HID CONNECTED (sub=%d proto=%d)", params.sub_class, params.proto);
      const hid_host_device_config_t cfg = {
        .callback = hid_host_interface_callback,
        .callback_arg = nullptr
      };
      esp_err_t er1 = hid_host_device_open(dev, &cfg);
      if (er1 != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_open: %s", esp_err_to_name(er1));
        return;
      }

      Serial.println("=== HID DEVICE ENUMERATED ===");
      Serial.printf("Subclass/Proto : %d / %d\n", params.sub_class, params.proto);
      Serial.println("=============================");

      esp_err_t er2 = hid_host_device_start(dev);
      if (er2 != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start: %s", esp_err_to_name(er2));
      } else {
        s_prev_bits = 0; // čistý start
      }
      break;
    }
    default:
      break;
  }
}

extern "C" void hid_host_device_callback(hid_host_device_handle_t dev,
                                         const hid_host_driver_event_t event,
                                         void *arg)
{
  hid_host_device_event(dev, event, arg);
}

// ---------- USB host task ----------
static void usb_host_task(void *pv)
{
  (void)pv;
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

// ---------- Arduino setup/loop ----------
void setup() {
  Serial.begin(115200);
  delay(150);

  // Verbózní logy (neexistující tagy se ignorují)
  esp_log_level_set("USB", ESP_LOG_DEBUG);
  esp_log_level_set("USBH", ESP_LOG_DEBUG);
  esp_log_level_set("HUB", ESP_LOG_DEBUG);
  esp_log_level_set("HID", ESP_LOG_DEBUG);
  esp_log_level_set(TAG, ESP_LOG_DEBUG);

  power_vbus_on();
  delay(150);

  // BLE gamepad init (20 buttons, bez Start/Select/Home)
  BleGamepadConfiguration gpCfg;
  gpCfg.setControllerType(CONTROLLER_TYPE_GAMEPAD);
  gpCfg.setButtonCount(20);
  gpCfg.setIncludeStart(false);
  gpCfg.setIncludeSelect(false);
  gpCfg.setIncludeHome(false);
  bleGamepad.begin(&gpCfg);

  // Default event loop
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    Serial.printf("esp_event_loop_create_default -> %s\n", esp_err_to_name(e));
  }

  // USB host
  usb_host_config_t host_cfg = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1
  };
  esp_err_t err = usb_host_install(&host_cfg);
  Serial.printf("usb_host_install -> %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  // HID host
  const hid_host_driver_config_t hid_cfg = {
    .create_background_task = true,
    .task_priority = 5,
    .stack_size = 6144,
    .core_id = 0,
    .callback = hid_host_device_callback,
    .callback_arg = nullptr
  };
  err = hid_host_install(&hid_cfg);
  Serial.printf("hid_host_install -> %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  // USB události
  xTaskCreatePinnedToCore(usb_host_task, "usb_host", 6144, nullptr, 2, nullptr, 0);

  Serial.println("Buzz -> BLE bridge ready");
}

void loop() {
  // vše běží v callbackách/taskách
}
