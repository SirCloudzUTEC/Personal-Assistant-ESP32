#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include "secrets.h"

// Cada pantalla va en su propio bus I2C, asi no importa que ambas tengan la direccion 0x3C
#define LEFT_SDA 8
#define LEFT_SCL 9
#define RIGHT_SDA 4
#define RIGHT_SCL 5

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

// Ubicacion para el clima (Lima por defecto) y zona horaria POSIX
#define LATITUDE "-12.05"
#define LONGITUDE "-77.04"
#define TIMEZONE "<-05>5"  // Peru, UTC-5 sin horario de verano

#define LATENCY_INTERVAL_MS 5000
#define WEATHER_INTERVAL_MS (10UL * 60UL * 1000UL)
#define LATENCY_HISTORY 20
#define FRAME_MS 30

Adafruit_SSD1306 leftDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SSD1306 rightDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);

// --- Monitoreo de red (lo actualiza networkTask en el otro nucleo) ---
int latencyHistory[LATENCY_HISTORY];  // ms, -1 = fallo
int latencyIndex = 0;
int latencyCount = 0;
volatile int disconnects = 0;

// --- Clima ---
float outdoorTemp = NAN;
int outdoorHumidity = -1;
int weatherCode = -1;
bool weatherFetched = false;

// --- Ojitos ---
struct Eyes {
  float x = 0, y = 0;              // desplazamiento actual de la mirada (-1..1)
  float targetX = 0, targetY = 0;  // a donde quiere mirar
  bool tracking = false;           // si true, la mirada la controla otra escena
  unsigned long nextMove = 0;
  unsigned long nextBlink = 0;
  unsigned long blinkStart = 0;
  bool blinking = false;
  bool happy = false;
  unsigned long happyUntil = 0;
} eyes;

// --- Pong ---
struct Pong {
  float ballX = 64, ballY = 32, vx = 2.2, vy = 1.3;
  float leftY = 24, rightY = 24;
  int leftScore = 0, rightScore = 0;
  unsigned long pausedUntil = 0;
} pong;

const int PADDLE_H = 16;

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) disconnects++;
}

void showMessage(const char *line1, const char *line2) {
  for (Adafruit_SSD1306 *d : {&leftDisplay, &rightDisplay}) {
    d->clearDisplay();
    d->setTextSize(1);
    d->setCursor(0, 0);
    d->println(line1);
    d->println(line2);
    d->display();
  }
}

void connectWiFi() {
  showMessage("Conectando a WiFi", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  disconnects = 0;  // no contar los intentos iniciales

  if (WiFi.status() == WL_CONNECTED) showMessage("Conectado!", WiFi.localIP().toString().c_str());
  else showMessage("Sin WiFi :(", "Reintentando luego");
  delay(1500);
}

// Mide la latencia a internet abriendo una conexion TCP al DNS de Cloudflare
int measureLatency() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClient client;
  unsigned long start = millis();
  if (!client.connect(IPAddress(1, 1, 1, 1), 53, 1000)) return -1;
  int elapsed = millis() - start;
  client.stop();
  return elapsed;
}

void updateLatency() {
  latencyHistory[latencyIndex] = measureLatency();
  latencyIndex = (latencyIndex + 1) % LATENCY_HISTORY;
  if (latencyCount < LATENCY_HISTORY) latencyCount++;
}

int lastLatency() {
  if (latencyCount == 0) return -1;
  return latencyHistory[(latencyIndex - 1 + LATENCY_HISTORY) % LATENCY_HISTORY];
}

void latencyStats(int &avg, int &lossPct) {
  int sum = 0, ok = 0;
  for (int i = 0; i < latencyCount; i++) {
    if (latencyHistory[i] >= 0) {
      sum += latencyHistory[i];
      ok++;
    }
  }
  avg = ok ? sum / ok : -1;
  lossPct = latencyCount ? (latencyCount - ok) * 100 / latencyCount : 0;
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(3000);
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE +
               "&longitude=" + LONGITUDE +
               "&current=temperature_2m,relative_humidity_2m,weather_code";
  http.begin(url);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      outdoorTemp = doc["current"]["temperature_2m"] | NAN;
      outdoorHumidity = doc["current"]["relative_humidity_2m"] | -1;
      weatherCode = doc["current"]["weather_code"] | -1;
      weatherFetched = true;
    }
  }
  http.end();
}

// Corre en el nucleo 0 para que las mediciones no traben las animaciones
void networkTask(void *) {
  unsigned long lastWeatherFetch = 0;
  for (;;) {
    updateLatency();
    // Reintenta cada minuto si todavia no hay clima
    unsigned long weatherInterval = weatherFetched ? WEATHER_INTERVAL_MS : 60000;
    if (lastWeatherFetch == 0 || millis() - lastWeatherFetch >= weatherInterval) {
      fetchWeather();
      lastWeatherFetch = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(LATENCY_INTERVAL_MS));
  }
}

const char *weatherText(int code) {
  if (code == 0) return "Despejado";
  if (code <= 3) return "Nublado";
  if (code <= 48) return "Neblina";
  if (code <= 57) return "Llovizna";
  if (code <= 67) return "Lluvia";
  if (code <= 77) return "Nieve";
  if (code <= 82) return "Chubascos";
  if (code <= 99) return "Tormenta";
  return "--";
}

// ======================= Pantallas de informacion =======================

void drawSignalBars(Adafruit_SSD1306 &d, int x, int y, int rssi) {
  int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    if (i < bars) d.fillRect(x + i * 4, y + 12 - h, 3, h, SSD1306_WHITE);
    else d.drawRect(x + i * 4, y + 12 - h, 3, h, SSD1306_WHITE);
  }
}

void drawNetScreen(Adafruit_SSD1306 &d) {
  d.setTextSize(1);
  d.setCursor(0, 0);

  if (WiFi.status() != WL_CONNECTED) {
    d.println("RED");
    d.println();
    d.println("WiFi desconectado");
    d.printf("Caidas: %d\n", disconnects);
    return;
  }

  int rssi = WiFi.RSSI();
  d.println(WiFi.SSID().substring(0, 15));
  drawSignalBars(d, 112, 0, rssi);

  d.drawFastHLine(0, 13, SCREEN_WIDTH, SSD1306_WHITE);
  d.setCursor(0, 16);
  d.printf("IP  %s\n", WiFi.localIP().toString().c_str());
  d.printf("Sen %d dBm  Ch %d\n", rssi, WiFi.channel());

  int avg, loss;
  latencyStats(avg, loss);
  int last = lastLatency();
  if (last >= 0) d.printf("Lat %d ms (~%d)\n", last, avg);
  else d.println("Lat  sin respuesta");
  d.printf("Perd %d%%  Caidas %d\n", loss, disconnects);

  // Mini grafico de latencia en la ultima fila
  int maxLat = 1;
  for (int i = 0; i < latencyCount; i++) maxLat = max(maxLat, latencyHistory[i]);
  for (int i = 0; i < latencyCount; i++) {
    int v = latencyHistory[(latencyIndex - latencyCount + i + LATENCY_HISTORY) % LATENCY_HISTORY];
    int x = 128 - (latencyCount - i) * 3;
    if (v < 0) d.drawPixel(x + 1, 63, SSD1306_WHITE);
    else {
      int h = max(1, v * 8 / maxLat);
      d.fillRect(x, 64 - h, 2, h, SSD1306_WHITE);
    }
  }
  d.setCursor(0, 56);
  d.printf("%lum", millis() / 60000);
}

void drawClockScreen(Adafruit_SSD1306 &d) {
  static const char *days[] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};
  static const char *months[] = {"Ene", "Feb", "Mar", "Abr", "May", "Jun",
                                 "Jul", "Ago", "Sep", "Oct", "Nov", "Dic"};
  struct tm t;
  if (getLocalTime(&t, 0)) {
    d.setTextSize(3);
    d.setCursor(1, 0);
    d.printf("%02d:%02d", t.tm_hour, t.tm_min);
    d.setTextSize(1);
    d.setCursor(94, 14);
    d.printf(":%02d", t.tm_sec);
    d.setCursor(0, 26);
    d.printf("%s %d %s %d", days[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900);
  } else {
    d.setTextSize(1);
    d.setCursor(0, 8);
    d.println("Sincronizando hora...");
  }

  d.drawFastHLine(0, 37, SCREEN_WIDTH, SSD1306_WHITE);
  d.setTextSize(1);
  d.setCursor(0, 41);
  if (weatherFetched) {
    d.printf("Afuera %.1f", outdoorTemp);
    d.print((char)247);  // simbolo de grados
    d.printf("C  %d%%\n", outdoorHumidity);
    d.println(weatherText(weatherCode));
  } else {
    d.println("Clima: cargando...");
    d.println();
  }
  d.printf("Chip %.0f", temperatureRead());
  d.print((char)247);
  d.print("C");
}

// ============================== Ojitos ==============================

void makeHappy(unsigned long now, unsigned long duration) {
  eyes.happy = true;
  eyes.happyUntil = now + duration;
}

void updateEyes(unsigned long now) {
  if (!eyes.tracking && now >= eyes.nextMove) {
    // A veces mira al centro, a veces a un lado
    if (random(3) == 0) {
      eyes.targetX = 0;
      eyes.targetY = 0;
    } else {
      eyes.targetX = random(-100, 101) / 100.0;
      eyes.targetY = random(-100, 101) / 100.0;
    }
    eyes.nextMove = now + random(800, 2500);
  }
  eyes.x += (eyes.targetX - eyes.x) * 0.25;
  eyes.y += (eyes.targetY - eyes.y) * 0.25;

  if (!eyes.blinking && now >= eyes.nextBlink) {
    eyes.blinking = true;
    eyes.blinkStart = now;
  }
  if (eyes.blinking && now - eyes.blinkStart > 180) {
    eyes.blinking = false;
    // A veces parpadea dos veces seguidas
    eyes.nextBlink = now + (random(4) == 0 ? 150 : random(2000, 5000));
  }

  if (!eyes.tracking && !eyes.happy && random(400) == 0) makeHappy(now, 1500);
  if (eyes.happy && now >= eyes.happyUntil) eyes.happy = false;
}

// Altura del ojo teniendo en cuenta el parpadeo (180ms: cierra y abre)
int blinkHeight(int h, unsigned long now) {
  if (!eyes.blinking) return h;
  float p = (now - eyes.blinkStart) / 180.0;
  float closed = p < 0.5 ? p * 2 : (1 - p) * 2;
  return max(4, (int)(h * (1 - closed)));
}

void drawEye(Adafruit_SSD1306 &d, int cx, int cy, int w, int h) {
  int r = min(w, h) / 4;
  d.fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, SSD1306_WHITE);
  if (eyes.happy) {
    // Tapa la parte de abajo para formar un ojito feliz ^ ^
    d.fillRoundRect(cx - w / 2 - 2, cy + h / 10, w + 4, h, r, SSD1306_BLACK);
  }
}

// Carita con dos ojos en una sola pantalla
void drawFace(Adafruit_SSD1306 &d, unsigned long now) {
  int h = blinkHeight(40, now);
  int cx = SCREEN_WIDTH / 2 + (int)(eyes.x * 10);
  int cy = SCREEN_HEIGHT / 2 + (int)(eyes.y * 5);
  drawEye(d, cx - 26, cy, 34, h);
  drawEye(d, cx + 26, cy, 34, h);
}

// Un ojo gigante por pantalla
void drawGiantEye(Adafruit_SSD1306 &d, unsigned long now) {
  int h = blinkHeight(54, now);
  int cx = SCREEN_WIDTH / 2 + (int)(eyes.x * 30);
  int cy = SCREEN_HEIGHT / 2 + (int)(eyes.y * 4);
  drawEye(d, cx, cy, 52, h);
}

// =============================== Pong ===============================

void resetBall(int direction) {
  pong.ballX = 64;
  pong.ballY = random(16, 48);
  pong.vx = 2.2 * direction;
  pong.vy = random(-15, 16) / 10.0;
  pong.pausedUntil = millis() + 600;
}

// La IA de cada paleta sigue la pelota con un poco de torpeza para que haya puntos
void movePaddle(float &paddleY, bool ballComing) {
  float target = (ballComing ? pong.ballY : 32) - PADDLE_H / 2 + random(-6, 7);
  float speed = ballComing ? 1.6 : 0.8;
  paddleY += constrain(target - paddleY, -speed, speed);
  paddleY = constrain(paddleY, 0, SCREEN_HEIGHT - PADDLE_H);
}

void updatePong(unsigned long now) {
  if (now >= pong.pausedUntil) {
    pong.ballX += pong.vx;
    pong.ballY += pong.vy;
    if (pong.ballY <= 0 || pong.ballY >= SCREEN_HEIGHT - 3) pong.vy = -pong.vy;

    if (pong.vx < 0 && pong.ballX <= 4 && pong.ballY + 3 >= pong.leftY && pong.ballY <= pong.leftY + PADDLE_H) {
      pong.vx = -pong.vx * 1.05;
      pong.vy += (pong.ballY - (pong.leftY + PADDLE_H / 2)) * 0.15;
    }
    if (pong.vx > 0 && pong.ballX >= 121 && pong.ballY + 3 >= pong.rightY && pong.ballY <= pong.rightY + PADDLE_H) {
      pong.vx = -pong.vx * 1.05;
      pong.vy += (pong.ballY - (pong.rightY + PADDLE_H / 2)) * 0.15;
    }
    pong.vx = constrain(pong.vx, -4.5, 4.5);
    pong.vy = constrain(pong.vy, -2.5, 2.5);

    // La carita va con la paleta izquierda: se pone feliz cuando gana un punto
    if (pong.ballX < -4) {
      pong.rightScore++;
      resetBall(1);
    } else if (pong.ballX > SCREEN_WIDTH + 4) {
      pong.leftScore++;
      makeHappy(now, 1200);
      resetBall(-1);
    }
  }
  movePaddle(pong.leftY, pong.vx < 0);
  movePaddle(pong.rightY, pong.vx > 0);

  // La carita sigue la pelota con la mirada (esta a la izquierda, asi que mira a la derecha)
  eyes.targetX = 0.2 + pong.ballX / SCREEN_WIDTH * 0.8;
  eyes.targetY = (pong.ballY - SCREEN_HEIGHT / 2) / (SCREEN_HEIGHT / 2);
}

void drawPong(Adafruit_SSD1306 &d) {
  for (int y = 0; y < SCREEN_HEIGHT; y += 6) d.drawFastVLine(64, y, 3, SSD1306_WHITE);
  d.fillRect(1, (int)pong.leftY, 3, PADDLE_H, SSD1306_WHITE);
  d.fillRect(124, (int)pong.rightY, 3, PADDLE_H, SSD1306_WHITE);
  d.fillRect((int)pong.ballX, (int)pong.ballY, 3, 3, SSD1306_WHITE);
  d.setTextSize(1);
  d.setCursor(50, 0);
  d.print(pong.leftScore);
  d.setCursor(72, 0);
  d.print(pong.rightScore);
}

// ============================== Escenas ==============================

typedef void (*SceneFn)(unsigned long now);

void sceneInfo(unsigned long) {
  drawNetScreen(leftDisplay);
  drawClockScreen(rightDisplay);
}

void sceneFaceNet(unsigned long now) {
  drawFace(leftDisplay, now);
  drawNetScreen(rightDisplay);
}

void scenePong(unsigned long now) {
  updatePong(now);
  drawFace(leftDisplay, now);
  drawPong(rightDisplay);
}

void sceneBigEyes(unsigned long now) {
  drawGiantEye(leftDisplay, now);
  drawGiantEye(rightDisplay, now);
}

void sceneClockFace(unsigned long now) {
  drawClockScreen(leftDisplay);
  drawFace(rightDisplay, now);
}

struct Scene {
  SceneFn draw;
  unsigned long durationMs;
  bool eyesTracking;
};

const Scene scenes[] = {
    {sceneInfo, 10000, false},
    {sceneFaceNet, 10000, false},
    {scenePong, 20000, true},
    {sceneBigEyes, 10000, false},
    {sceneClockFace, 10000, false},
};
const int SCENE_COUNT = sizeof(scenes) / sizeof(scenes[0]);
int currentScene = 0;
unsigned long sceneStart = 0;

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  Wire.begin(LEFT_SDA, LEFT_SCL);
  Wire1.begin(RIGHT_SDA, RIGHT_SCL);
  if (!leftDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR) ||
      !rightDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("No se pudo iniciar alguna pantalla SSD1306");
    while (true) delay(1000);
  }
  leftDisplay.setTextColor(SSD1306_WHITE);
  rightDisplay.setTextColor(SSD1306_WHITE);

  connectWiFi();
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(networkTask, "network", 8192, nullptr, 1, nullptr, 0);

  sceneStart = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - sceneStart >= scenes[currentScene].durationMs) {
    currentScene = (currentScene + 1) % SCENE_COUNT;
    sceneStart = now;
    if (scenes[currentScene].draw == scenePong) resetBall(random(2) ? 1 : -1);
  }
  eyes.tracking = scenes[currentScene].eyesTracking;
  updateEyes(now);

  leftDisplay.clearDisplay();
  rightDisplay.clearDisplay();
  scenes[currentScene].draw(now);
  leftDisplay.display();
  rightDisplay.display();

  long wait = FRAME_MS - (long)(millis() - now);
  if (wait > 0) delay(wait);
}
