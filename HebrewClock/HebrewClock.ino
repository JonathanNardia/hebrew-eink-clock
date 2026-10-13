#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "epd_driver.h"
#include "hebrew_words.h"
#include "secrets.h"

// LILYGO T5 4.7 S3, 960x540; Arduino ESP32 core 2.0.17.
// First version is intended for USB power (no deep sleep).
uint8_t *framebuffer = nullptr;
uint8_t *previousFrame = nullptr;
uint8_t *changeMask = nullptr;
constexpr size_t FRAME_BYTES = EPD_WIDTH * EPD_HEIGHT / 2;
// Full clean refresh (with a flash) once a night to clear ghosting.
constexpr int CLEAN_HOUR = 3;
bool hasPreviousFrame = false;
const char *TZ_ISRAEL = "IST-2IDT,M3.4.4/26,M10.5.0";
// Vocalized text. Minute forms already carry the conjunction vav with the
// right vowel (וְ / וּ before a shva or ב,מ,פ / וַ before a hataf).
// After editing any Hebrew text here, run tools/make_words.py.
const char *EXACT = "בְּדִיּוּק";
const char *hours[] = {"שְׁתֵּים עֶשְׂרֵה", "אַחַת", "שְׁתַּיִם", "שָׁלֹשׁ", "אַרְבַּע", "חָמֵשׁ", "שֵׁשׁ", "שֶׁבַע", "שְׁמוֹנֶה", "תֵּשַׁע", "עֶשֶׂר", "אַחַת עֶשְׂרֵה"};
const char *unitsV[] = {"", "וְאַחַת", "וּשְׁתַּיִם", "וְשָׁלֹשׁ", "וְאַרְבַּע", "וְחָמֵשׁ", "וְשֵׁשׁ", "וְשֶׁבַע", "וּשְׁמוֹנֶה", "וְתֵשַׁע"};
const char *teensV[] = {"וְעֶשֶׂר", "וְאַחַת עֶשְׂרֵה", "וּשְׁתֵּים עֶשְׂרֵה", "וּשְׁלֹשׁ עֶשְׂרֵה", "וְאַרְבַּע עֶשְׂרֵה", "וַחֲמֵשׁ עֶשְׂרֵה", "וְשֵׁשׁ עֶשְׂרֵה", "וּשְׁבַע עֶשְׂרֵה", "וּשְׁמוֹנֶה עֶשְׂרֵה", "וּתְשַׁע עֶשְׂרֵה"};
const char *tensV[] = {"", "", "וְעֶשְׂרִים", "וּשְׁלֹשִׁים", "וְאַרְבָּעִים", "וַחֲמִשִּׁים"};

String minuteWords(int m) {
  if (m == 0) return EXACT;
  if (m < 10) return unitsV[m];
  if (m < 20) return teensV[m - 10];
  String result = tensV[m / 10];
  if (m % 10) result += String(" ") + unitsV[m % 10];
  return result;
}

// Words are pre-shaped bitmaps (letters + nikud) from hebrew_words.h.
const HebrewWord *findWord(const String &word) {
  for (size_t i = 0; i < HEBREW_WORD_COUNT; ++i)
    if (word == HEBREW_WORDS[i].text) return &HEBREW_WORDS[i];
  Serial.printf("Missing word bitmap: %s (run tools/make_words.py)\n", word.c_str());
  return nullptr;
}

// Splits on spaces; returns the words found, in reading order.
int splitWords(const String &text, const HebrewWord **out, int maxWords) {
  int count = 0, from = 0;
  while (from < (int)text.length() && count < maxWords) {
    int space = text.indexOf(' ', from);
    if (space < 0) space = text.length();
    if (space > from) {
      const HebrewWord *w = findWord(text.substring(from, space));
      if (w) out[count++] = w;
    }
    from = space + 1;
  }
  return count;
}

void drawHebrew(const String &text, int top) {
  const HebrewWord *words[8];
  int count = splitWords(text, words, 8);
  int total = SPACE_WIDTH * (count > 0 ? count - 1 : 0);
  for (int i = 0; i < count; ++i) total += words[i]->advance;

  // Right-to-left: first word at the right edge of the centered line.
  int right = (EPD_WIDTH + total) / 2;
  for (int i = 0; i < count; ++i) {
    const HebrewWord &w = *words[i];
    right -= w.advance;
    for (int y = 0; y < WORD_HEIGHT; ++y) {
      for (int x = 0; x < w.width; ++x) {
        uint32_t bit = y * w.width + x;
        if (pgm_read_byte(w.data + bit / 8) & (0x80 >> (bit % 8)))
          epd_draw_pixel(right + x, top + y, 0, framebuffer);
      }
    }
    right -= SPACE_WIDTH;
  }
}

void refreshDisplay(bool fullClean = false) {
  if (!fullClean && hasPreviousFrame && memcmp(previousFrame, framebuffer, FRAME_BYTES) == 0)
    return;

  epd_poweron();
  if (fullClean || !hasPreviousFrame) {
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  } else {
    // The image contains only 0x0 (black) and 0xF (white) nibbles.
    // Mask nibble 0 drives a pixel; 0xF leaves it untouched in these modes.
    // WHITE_ON_WHITE erases black-to-white transitions using white pulses.
    bool needsErase = false;
    for (size_t i = 0; i < FRAME_BYTES; ++i) {
      changeMask[i] = previousFrame[i] | uint8_t(~framebuffer[i]);
      needsErase |= changeMask[i] != 0xFF;
    }
    // Two erase passes: one leaves visible ghosts of the old words.
    if (needsErase)
      for (int pass = 0; pass < 2; ++pass)
        epd_draw_image(epd_full_screen(), changeMask, WHITE_ON_WHITE);

    // Only white-to-black transitions get black pulses. Shared letter
    // strokes and the background keep their existing state.
    bool needsDraw = false;
    for (size_t i = 0; i < FRAME_BYTES; ++i) {
      changeMask[i] = uint8_t(~previousFrame[i]) | framebuffer[i];
      needsDraw |= changeMask[i] != 0xFF;
    }
    if (needsDraw)
      epd_draw_image(epd_full_screen(), changeMask, BLACK_ON_WHITE);
  }
  // Drop all panel lines; plain epd_poweroff() can leave a faint stripe.
  epd_poweroff_all();
  memcpy(previousFrame, framebuffer, FRAME_BYTES);
  hasPreviousFrame = true;
}

void show(const String &line1, const String &line2, bool fullClean = false) {
  memset(framebuffer, 0xFF, FRAME_BYTES);
  int top1 = (EPD_HEIGHT - 2 * WORD_HEIGHT - LINE_GAP) / 2;
  drawHebrew(line1, top1);
  drawHebrew(line2, top1 + WORD_HEIGHT + LINE_GAP);
  refreshDisplay(fullClean);
}

void setup() {
  Serial.begin(115200);
  epd_init();
  framebuffer = (uint8_t *)ps_malloc(FRAME_BYTES);
  previousFrame = (uint8_t *)ps_malloc(FRAME_BYTES);
  changeMask = (uint8_t *)ps_malloc(FRAME_BYTES);
  if (!framebuffer || !previousFrame || !changeMask) {
    Serial.println("PSRAM allocation failed. Enable OPI PSRAM.");
    while (true) delay(1000);
  }
  show("מִתְחַבֵּר לָרֶשֶׁת", "מַמְתִּין לַשָּׁעָה");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TZ_ISRAEL, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

void loop() {
  static time_t lastMinute = -1;
  static uint32_t lastReconnect = 0;
  static bool waitingShown = false;
  static bool timeShown = false;
  time_t now = time(nullptr);
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 30000) {
    lastReconnect = millis();
    WiFi.reconnect();
  }
  if (now < 1700000000) {
    if (millis() > 45000 && !waitingShown) {
      show("מַמְתִּין לַשָּׁעָה", "בְּדֹק חִבּוּר לָרֶשֶׁת");
      waitingShown = true;
    }
    delay(200);
    return;
  }
  if (now / 60 != lastMinute) {
    lastMinute = now / 60;
    struct tm local;
    localtime_r(&now, &local);
    // Full clean on the first real time too, to wipe the boot messages.
    bool clean = !timeShown || (local.tm_hour == CLEAN_HOUR && local.tm_min == 0);
    show(hours[local.tm_hour % 12], minuteWords(local.tm_min), clean);
    timeShown = true;
    Serial.printf("%02d:%02d\n", local.tm_hour, local.tm_min);
  }
  delay(200);
}
