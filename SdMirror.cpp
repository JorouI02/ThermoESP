// SdMirror.cpp
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <LittleFS.h>

// --- Пинове за SD (както ги ползваш) ---
static const int SD_CS   = 16;
static const int SD_SCK  = 32;
static const int SD_MOSI = 25;
static const int SD_MISO = 23;

// Огледалвани файлове
static const char* SRC1 = "/thermo_log.csv";
static const char* DST1 = "/thermo_log.csv";
static const char* SRC2 = "/thermo_pretty.log";
static const char* DST2 = "/thermo_pretty.log";

// Настройки
static const uint32_t SD_FREQ_FAST = 10000000; // 10 MHz
static const uint32_t SD_FREQ_SAFE =  4000000; // 4  MHz
static const uint32_t FIRST_DELAY_MS = 5000;   // изчакай преди първо огледало
static const uint32_t MIRROR_PERIOD_MS = 30000;

static SPIClass sdSPI(HSPI);
static bool sd_ready = false;
static unsigned long t0 = 0;
static unsigned long lastMirror = 0;

// Монтаж с ретраи и два честотни опита
static bool mountSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (SD.begin(SD_CS, sdSPI, SD_FREQ_FAST)) return true;
  delay(100);
  return SD.begin(SD_CS, sdSPI, SD_FREQ_SAFE);
}

// Копиране само ако източникът е по-дълъг (append-only). Ако SD е по-дълъг, презапиши от източника.
static void mirrorFileAppendOnly(const char* srcPath, const char* dstPath) {
  if (!LittleFS.exists(srcPath)) return;

  File src = LittleFS.open(srcPath, FILE_READ);
  if (!src) return;

  size_t srcSize = src.size();
  if (srcSize == 0) { src.close(); return; } // не копирай празно

  if (!SD.exists(dstPath)) {
    File nf = SD.open(dstPath, FILE_WRITE); // създай празен файл на SD
    if (nf) nf.close();
  }

  File dst = SD.open(dstPath, FILE_APPEND);
  if (!dst) { src.close(); sd_ready = false; return; }

  size_t dstSize = dst.size();

  if (dstSize > srcSize) {
    // SD е по-дълъг (напр. стар файл) -> пълно презаписване от източника
    dst.close();
    dst = SD.open(dstPath, FILE_WRITE); // truncate
    if (!dst) { src.close(); sd_ready = false; return; }
    src.seek(0);
  } else {
    // Нормално: допиши новите байтове
    src.seek(dstSize);
  }

  uint8_t buf[1024];
  while (true) {
    size_t n = src.read(buf, sizeof(buf));
    if (n == 0) break;
    dst.write(buf, n);
  }
  dst.flush();
  dst.close();
  src.close();
}

// Тези две функции ги викаш от .ino (вече имаш декларации там)
void setupSdMirror() {
  // малко време след boot и след твоите първи записи
  t0 = millis();
  sd_ready = mountSD(); // опит веднага; ако не стане, loop ще ретрайнe
}

void loopSdMirror() {
  // Първо изчакай да мине FIRST_DELAY_MS, за да са готови файловете в LittleFS
  if (millis() - t0 < FIRST_DELAY_MS) return;

  if (!sd_ready) {
    sd_ready = mountSD();
    if (!sd_ready) return;
  }

  unsigned long now = millis();
  if (now - lastMirror >= MIRROR_PERIOD_MS) {
    lastMirror = now;
    mirrorFileAppendOnly(SRC1, DST1);
    mirrorFileAppendOnly(SRC2, DST2);
  }
}
