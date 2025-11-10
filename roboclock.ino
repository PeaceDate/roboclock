#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>
#include <time.h>
#include <Preferences.h>

// ===== WiFi info =====
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ===== OLED setup =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ===== SHT31 =====
Adafruit_SHT31 sht31 = Adafruit_SHT31();
bool hasRealSht31 = false;

// ===== Pins =====
#define I2C_SDA 8
#define I2C_SCL 9
#define LED_PIN 4
#define LED_LAN 5
#define WARN_LED_PIN 2
#define BTN_TZ 7

// ===== LED / warning =====
unsigned long lastBlink     = 0;

int fadeValue = 0;
int fadeStep  = 5;

// Safe sensor ranges
float TEMP_MIN = 18.0;
float TEMP_MAX = 30.0;
float HUM_MIN  = 30.0;
float HUM_MAX  = 70.0;

// ---- Фейковий датчик для Wokwi ----
void readFakeSht31(float &tempC, float &hum) {
  static unsigned long t0 = millis();
  unsigned long t = millis() - t0;

  tempC = 25.0 + 10.0 * sin(t / 50000.0);
  hum   = 50.0 + 10.0 * sin(t / 60000.0);
}

// ==== LOADER (залишив на всякий випадок) ====
const uint8_t loaderFrames[4][8] = {
  { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 },
  { 0x10, 0x18, 0x0c, 0x06, 0x06, 0x0c, 0x18, 0x10 },
  { 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00 },
  { 0x06, 0x0c, 0x18, 0x30, 0x30, 0x18, 0x0c, 0x06 }
};

// ===== Timezones =====
struct Timezone {
  const char* shortName;
  const char* tzString;
};

Timezone timezones[] = {
  { "UTC", "UTC0" },
  { "UA",  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "UK",  "GMT0BST,M3.5.0/1,M10.5.0/2" },
  { "CET", "CET-1CEST,M3.5.0/2,M10.5.0/3" },
  { "MSK", "MSK-3" },
  { "EST", "EST5EDT,M3.2.0/2,M11.1.0/2" },
  { "CST", "CST6CDT,M3.2.0/2,M11.1.0/2" },
  { "PST", "PST8PDT,M3.2.0/2,M11.1.0/2" },
  { "JST", "JST-9" },
  { "IST", "IST-5:30" }
};

int currentTzIndex = 1; // "UA" за замовчуванням

Preferences prefs;

// Кнопка (debounce)
bool lastBtnState      = HIGH;
unsigned long lastBtnChange = 0;

// Інфо-текст згори
uint8_t infoMode = 0;                  // 0=date,1=phase,2=TZ,3=OK
unsigned long lastInfoChange = 0;

// Дні тижня (робот-стайл)
const char* WEEK_DAYS[7] = {
  "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
  "THURSDAY", "FRIDAY", "SATURDAY"
};

// ===== NTP / час =====
bool timeSynced = false;
unsigned long lastTimeRetry = 0;

// ===== Marquee =====
String marqueeText = "";
int marqueeX = 0;
int marqueeY = 0;
int marqueeWidth = 0;
unsigned long lastMarqueeStep = 0;
const uint16_t MARQUEE_INTERVAL = 20;
bool marqueeActive = false;
bool marqueeLoop   = true;     // true → безкінечно, false → один прохід

// спец-флаг: зараз крутиться повідомлення про зміну TZ
bool tzChangeMarqueeActive = false;

// ===== Warning intro "ALERT" (2 блимання) =====
bool warningIntroActive = false;
uint8_t warningIntroPhase = 0;               // 0=нема, 1=ALERT,2=порожньо,3=ALERT,4=порожньо
unsigned long lastWarningIntroToggle = 0;
const uint16_t WARNING_ALERT_BLINK_INTERVAL = 250; // мс на фазу
bool prevWarning = false;

// ---------- helpers ----------

bool isTimeValid() {
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  return (ti.tm_year > 120);
}

// Блочне очікування NTP з marquee-повідомленням
bool waitForTime(uint32_t timeoutMs, const char* loaderMsg) {
  Serial.print("Waiting for time");
  uint32_t start = millis();

  bool useMarquee = (loaderMsg != nullptr);
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  int16_t x = 0;

  if (useMarquee) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.getTextBounds(loaderMsg, 0, 0, &x1, &y1, &w, &h);
    x = display.width(); // старт за правим краєм (для rotation(1) це 32)
  }

  bool timeBecameValid = false;

  while (millis() - start < timeoutMs) {
    if (!timeBecameValid && isTimeValid()) {
      timeBecameValid = true;
      Serial.println("\n--- Time synced! ---");

      if (!useMarquee) {
        return true;
      }
      // якщо крутимо текст – даємо йому докрутитись один раз
    }

    if (useMarquee) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(x, 8);
      display.print(loaderMsg);
      display.display();

      x--;

      if (x < -((int)w)) {
        if (timeBecameValid) {
          return true;
        } else {
          x = display.width();
        }
      }
    }

    Serial.print(".");
    delay(20);
  }

  if (timeBecameValid) {
    return true;
  }

  Serial.println("\n!!! NTP timeout, no time yet");
  return false;
}

// ===== Marquee control =====
void startMarquee(const char* txt, int y = 8, bool loop = true) {
  marqueeText   = txt;
  marqueeActive = true;
  marqueeY      = y;
  marqueeLoop   = loop;

  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.getTextBounds(marqueeText, 0, 0, &x1, &y1, &w, &h);
  marqueeWidth = w;
  marqueeX     = display.width();
  lastMarqueeStep = millis();
}

void stopMarquee() {
  marqueeActive = false;
  marqueeText   = "";
  marqueeLoop   = true;
  tzChangeMarqueeActive = false;  // якщо це було TZ-повідомлення, воно вже закінчилось
}

// ===== TZ =====
void applyTimezone() {
  setenv("TZ", timezones[currentTzIndex].tzString, 1);
  tzset();
}

void handleTimezoneButton() {
  int reading = digitalRead(BTN_TZ);

  if (reading != lastBtnState && (millis() - lastBtnChange) > 200) {
    lastBtnChange = millis();

    if (reading == LOW) {
      currentTzIndex++;
      int tzCount = sizeof(timezones) / sizeof(timezones[0]);
      if (currentTzIndex >= tzCount) currentTzIndex = 0;

      applyTimezone();
      prefs.putInt("tzIndex", currentTzIndex);

      Serial.print("Switched TZ to: ");
      Serial.println(timezones[currentTzIndex].shortName);

      // якщо часу ще нема – спроба досинхронізувати з власним текстом
      if (!timeSynced) {
        timeSynced = waitForTime(
          5000,
          "REALIGNING TO NEW PLANETARY TIME GRID..."
        );
      }

      // зупиняємо possible ALERT intro, щоб TZ-повідомлення не блокувалось
      warningIntroActive = false;
      warningIntroPhase  = 0;

      // показуємо юзеру, на що змінилась таймзона (один прохід)
      char msg[40];
      snprintf(
        msg,
        sizeof(msg),
        "TIME GRID UPDATED: SECTOR %s",
        timezones[currentTzIndex].shortName
      );
      tzChangeMarqueeActive = true;
      startMarquee(msg, 5, false);   // один прохід
    }
  }

  lastBtnState = reading;
}

// Роботський опис фази доби за годиною в 24-год форматі
String getPhaseString(int hours24) {
  if (hours24 < 5) {
    return "LOCAL PHASE: DEEP NIGHT CYCLE.";
  } else if (hours24 < 11) {
    return "LOCAL PHASE: MORNING SCAN CYCLE.";
  } else if (hours24 < 17) {
    return "LOCAL PHASE: DAYLIGHT OPERATION CYCLE.";
  } else if (hours24 < 21) {
    return "LOCAL PHASE: EVENING WIND-DOWN CYCLE.";
  } else {
    return "LOCAL PHASE: NIGHT WATCH CYCLE.";
  }
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  prefs.begin("clock", false);
  int savedIndex = prefs.getInt("tzIndex", -1);
  int tzCount = sizeof(timezones) / sizeof(timezones[0]);
  if (savedIndex >= 0 && savedIndex < tzCount) {
    currentTzIndex = savedIndex;
  }

  WiFi.begin(ssid, password, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n--- WiFi connected! ---");

  Serial.print("Waiting for IP");
  while (WiFi.localIP().toString() == "0.0.0.0") {
    Serial.print(".");
    delay(250);
  }
  Serial.print("  IP: ");
  Serial.println(WiFi.localIP());

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.clearDisplay();
  display.setRotation(1);  // 128x32 → 32x128
  display.setTextWrap(false);
  display.display();

  if (!sht31.begin(0x44)) {
    Serial.println("Couldn't find SHT31, using FAKE data");
    hasRealSht31 = false;
  } else {
    hasRealSht31 = true;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  applyTimezone();

  // початкове очікування часу з роботським текстом
  timeSynced = waitForTime(
    20000,
    "UNIT ONLINE... ESTABLISHING LINK TO ORBITAL TIME BEACON..."
  );

  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_LAN, OUTPUT);
  pinMode(WARN_LED_PIN, OUTPUT);
  pinMode(BTN_TZ, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_LAN, LOW);
}

void loop() {
  unsigned long now = millis();

  // періодичний retry з власним marquee-текстом
  if (!timeSynced && (now - lastTimeRetry > 60000)) {
    lastTimeRetry = now;
    Serial.println("Retry NTP sync...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    applyTimezone();
    timeSynced = waitForTime(
      5000,
      "RETRYING LINK TO ORBITAL TIME BEACON..."
    );
  }

  // рух глобального marquee
  if (marqueeActive && (now - lastMarqueeStep > MARQUEE_INTERVAL)) {
    lastMarqueeStep = now;
    marqueeX--;
    if (marqueeX < -marqueeWidth) {
      if (marqueeLoop) {
        marqueeX = display.width();   // нескінченний цикл
      } else {
        stopMarquee();                // один прохід → стоп
      }
    }
  }

  handleTimezoneButton();

  time_t nowUnix = time(nullptr);
  struct tm timeinfo;
  localtime_r(&nowUnix, &timeinfo);

  int hours24   = timeinfo.tm_hour;
  int minutes   = timeinfo.tm_min;
  int seconds   = timeinfo.tm_sec;
  int dayOfWeek = timeinfo.tm_wday;
  int day       = timeinfo.tm_mday;
  int month     = timeinfo.tm_mon + 1;
  int year      = timeinfo.tm_year + 1900;

  int hours = hours24;
  bool isPM = false;
  if (hours >= 12) {
    isPM = true;
    if (hours > 12) hours -= 12;
  }
  if (hours == 0) hours = 12;

  float tempC, hum;
  if (hasRealSht31) {
    tempC = sht31.readTemperature();
    hum   = sht31.readHumidity();
  } else {
    readFakeSht31(tempC, hum);
  }

  // ---- детальна діагностика WARNING ----
  bool tempTooLow  = (tempC < TEMP_MIN);
  bool tempTooHigh = (tempC > TEMP_MAX);
  bool humTooLow   = (hum   < HUM_MIN);
  bool humTooHigh  = (hum   > HUM_MAX);

  bool warning = tempTooLow || tempTooHigh || humTooLow || humTooHigh;

  // ===== Запуск / оновлення intro "ALERT" =====
  if (warning && !prevWarning) {
    // новий warning, але не перебиваємо повідомлення про зміну TZ
    if (!tzChangeMarqueeActive) {
      warningIntroActive = true;
      warningIntroPhase  = 1;           // старт з "ALERT" ON
      lastWarningIntroToggle = now;
    }
  }
  if (!warning) {
    warningIntroActive = false;
    warningIntroPhase  = 0;
  }
  prevWarning = warning;

  if (warningIntroActive) {
    if (now - lastWarningIntroToggle >= WARNING_ALERT_BLINK_INTERVAL) {
      lastWarningIntroToggle = now;
      warningIntroPhase++;
      if (warningIntroPhase > 4) {
        // ALERT показався двічі → завершуємо intro
        warningIntroActive = false;
        warningIntroPhase  = 0;
      }
    }
  }

  if (warning) {
    fadeValue += fadeStep;
    if (fadeValue <= 0 || fadeValue >= 255) fadeStep = -fadeStep;
    analogWrite(WARN_LED_PIN, fadeValue);
  } else {
    analogWrite(WARN_LED_PIN, 0);
  }

  // інфо-режим крутиться завжди, якщо немає warning
  if (!warning && (now - lastInfoChange >= 10000)) {
    lastInfoChange = now;
    infoMode = (infoMode + 1) % 4;   // 0..3
  }

  char hStr[3], mStr[3], sStr[3];
  if (timeSynced) {
    sprintf(hStr, "%02d", hours);
    sprintf(mStr, "%02d", minutes);
    sprintf(sStr, "%02d", seconds);
  } else {
    strcpy(hStr, "--");
    strcpy(mStr, "--");
    strcpy(sStr, "--");
  }

  // ===== Вибір marquee (пріоритет: WARNING > !timeSynced > інфо) =====
  String desiredMarqueeStr = "";
  bool   desiredLoop       = true;   // за замовчуванням крутити в циклі

  if (warning) {
    // формуємо роботський текст з описом, що саме не так
    String warnStr = "ERR CODE:";
    bool first = true;

    if (tempTooLow) {
      if (!first) warnStr += " |";
      warnStr += " TEMP TOO LOW";
      first = false;
    }
    if (tempTooHigh) {
      if (!first) warnStr += " |";
      warnStr += " TEMP TOO HIGH";
      first = false;
    }
    if (humTooLow) {
      if (!first) warnStr += " |";
      warnStr += " HUMIDITY TOO LOW";
      first = false;
    }
    if (humTooHigh) {
      if (!first) warnStr += " |";
      warnStr += " HUMIDITY TOO HIGH";
      first = false;
    }

    warnStr += " | ADJUST HABITAT CONDITIONS.";

    desiredMarqueeStr = warnStr;
    desiredLoop       = true;        // попередження – безкінечно
  } else if (!timeSynced) {
    desiredMarqueeStr =
      "TEMPORAL LINK LOST. RE-ACQUIRING PLANETARY TIME SIGNAL...";
    desiredLoop = true;
  } else if (infoMode == 0) {
    // дата – один прохід
    char buf[48];
    const char* dow = WEEK_DAYS[constrain(dayOfWeek, 0, 6)];
    snprintf(buf, sizeof(buf),
             "SCAN DATE: %s %02u.%02u.%04u",
             dow, day, month, year);
    desiredMarqueeStr = String(buf);
    desiredLoop       = false;
  } else if (infoMode == 1) {
    // Фаза доби – один прохід
    desiredMarqueeStr = getPhaseString(hours24);
    desiredLoop       = false;
  } else if (infoMode == 2) {
    // TIMEZONE – один прохід
    desiredMarqueeStr =
      String("ACTIVE TIME GRID: SECTOR ") +
      timezones[currentTzIndex].shortName;
    desiredLoop = false;
  } else if (infoMode == 3) {
    // статус – один прохід
    desiredMarqueeStr =
      "ALL SYSTEMS NOMINAL. CONTINUING HABITAT MONITORING.";
    desiredLoop = false;
  }

  // логіка увімкнення marquee:
  // - якщо warning → раніше ми перебивали все, але тепер НЕ перебиваємо
  //   активне однопрохідне TZ-повідомлення
  if (desiredMarqueeStr.length() > 0) {
    if (!marqueeActive) {
      startMarquee(desiredMarqueeStr.c_str(), 5, desiredLoop);
    } else {
      // якщо крутиться повідомлення про зміну TZ (one-shot) – не чіпаємо його
      if (tzChangeMarqueeActive && !marqueeLoop) {
        // даємо TZ-тексту доїхати
      } else {
        bool isWarningDesired = warning;
        if (marqueeText != desiredMarqueeStr &&
            (marqueeLoop || isWarningDesired)) {
          startMarquee(desiredMarqueeStr.c_str(), 5, desiredLoop);
        }
      }
    }
  } else {
    // немає бажаного тексту по логіці – зупиняємо тільки циклічні
    if (marqueeActive && marqueeLoop) {
      stopMarquee();
    }
  }

  // ===== OLED =====
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Верхня зона
  if (warningIntroActive && !tzChangeMarqueeActive) {
    // під час intro ми показуємо тільки мигаючий ALERT
    if (warningIntroPhase == 1 || warningIntroPhase == 3) {
      display.setCursor(1, 5); // приблизно по центру 32px висоти
      display.print("ALERT");
    }
  } else if (marqueeActive) {
    display.setCursor(marqueeX, marqueeY);
    display.print(marqueeText);
  } else {
    int x = 4;
    int y = 5;
    display.setCursor(x, y);

    if (!timeSynced) {
      display.print("TEMPORAL ERROR");
    }
  }

  display.drawLine(0, 18, 128, 18, SSD1306_WHITE);

  // Час (HH MM SS вертикально)
  display.setTextSize(2);
  display.setCursor(5, 32);
  display.print(hStr);
  display.setCursor(5, 52);
  display.print(mStr);
  display.setCursor(5, 72);
  display.print(sStr);

  // Температура / вологість
  display.drawLine(0, 98, 128, 98, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(5, 105);
  display.print(int(tempC));
  display.print("C ");
  display.drawCircle(24, 105, 1, 1);
  display.setCursor(6, 118);
  display.print(int(hum));
  display.print("%");

  display.display();

  // ===== Blink LEDs every 5 sec =====
  if (now - lastBlink >= 5000) {
    digitalWrite(LED_PIN, HIGH);
    delay(20);
    digitalWrite(LED_PIN, LOW);

    delay(20);

    digitalWrite(LED_LAN, HIGH);
    delay(20);
    digitalWrite(LED_LAN, LOW);

    lastBlink = now;
  }

  delay(30);
}