#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>

#include "AudioTask.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kScreenW = 240;
constexpr int kScreenH = 135;
constexpr int kArenaTop = 16;
constexpr int kArenaLeft = 4;
constexpr int kArenaRight = kScreenW - 4;
constexpr int kPaddleY = 120;
constexpr uint32_t kTitleScreenMs = 5000;
constexpr uint32_t kScoreScreenMs = 4000;
constexpr uint32_t kKonamiFirstRevealMs = 4000;
constexpr uint32_t kKonamiSecondRevealMs = 6000;
constexpr uint32_t kGyroOffScreenMs = 2200;
constexpr uint8_t kSfxVirtualChannel = 0;
constexpr uint8_t kMp3VirtualChannel = 1;

constexpr uint8_t HID_V = 0x19;

constexpr size_t kMaxBricks = 60;
constexpr size_t kMaxBalls = 3;
constexpr size_t kMaxPowerUps = 4;
constexpr size_t kMaxLasers = 6;
constexpr size_t kMaxTrail = 10;
constexpr size_t kMaxParticles = 48;
constexpr size_t kFireBufferSize = (kScreenW * (kScreenH + 2)) + 2;
constexpr size_t kScoreCount = 5;
constexpr size_t kOptionCount = 14;
constexpr size_t kBackgroundCount = 12;
constexpr size_t kKonamiCodeLength = 11;
constexpr float kGyroTurnSign = -1.0f;
constexpr char kNameCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
constexpr float kGyroSensitivityBoost = 1.30f;
constexpr float kGyroTurnDeadzone = 35.0f;
constexpr float kGyroTurnFullScale = 140.0f;
constexpr float kGyroAccelerationStrength = 0.85f;
constexpr float kMotionLaunchThreshold = 0.42f;
constexpr float kMotionFilterBlend = 0.18f;
constexpr uint32_t kMotionLaunchCooldownMs = 350;

constexpr char kDataDir[] = "/ADVnoid";
constexpr char kConfigFile[] = "/ADVnoid/config.txt";
constexpr char kScoreFile[] = "/ADVnoid/highscores.txt";

enum class GameState : uint8_t {
  Title,
  Help,
  Playing,
  BackgroundSelect,
  Pause,
  Options,
  Playlist,
  LevelClear,
  NameEntry,
  GameOver,
  KonamiReveal,
  GyroOff,
};

enum class KonamiToken : uint8_t {
  Up,
  Down,
  Left,
  Right,
  B,
  A,
  Enter,
};

enum class PowerType : uint8_t {
  Expand,
  Laser,
  Slow,
  MultiBall,
};

enum class ImageFormat : uint8_t {
  Png,
  Jpg,
};

struct Brick {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  float baseX = 0.0f;
  float motionRange = 0.0f;
  float motionSpeed = 0.0f;
  float motionPhase = 0.0f;
  uint16_t color = 0;
  uint8_t colorVariant = 0;
  uint8_t hits = 0;
  bool breakable = true;
  bool alive = false;
};

struct Ball {
  bool active = false;
  float x = 0.0f;
  float y = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float r = 3.4f;
  bool stuck = true;
};

struct Paddle {
  float x = 0.0f;
  float w = 42.0f;
};

struct PowerUp {
  bool active = false;
  PowerType type = PowerType::Expand;
  float x = 0.0f;
  float y = 0.0f;
  float vy = 46.0f;
};

struct LaserShot {
  bool active = false;
  float x = 0.0f;
  float y = 0.0f;
  float vy = -220.0f;
};

struct Particle {
  bool active = false;
  float x = 0.0f;
  float y = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float life = 0.0f;
  float maxLife = 0.0f;
  uint16_t color = 0;
};

struct TrailPoint {
  float x = 0.0f;
  float y = 0.0f;
  bool active = false;
};

struct ScoreEntry {
  char name[4] = {'-', '-', '-', '\0'};
  uint32_t score = 0;
};

struct GameConfig {
  bool sfx = true;
  uint8_t volumeStep = 6;
  uint8_t difficulty = 1;
  bool trails = true;
  uint8_t paddleSize = 1;
  bool randomBackground = false;
  uint8_t backgroundIndex = 0;
  bool mp3Enabled = false;
  uint8_t mp3VolumeStep = 5;
  char leftKey = 'a';
  char rightKey = 'd';
  char actionKey = 'p';
};

struct BackgroundAsset {
  const uint8_t* start;
  const uint8_t* end;
  const char* name;
  ImageFormat format;

  constexpr BackgroundAsset(const uint8_t* startPtr, const uint8_t* endPtr,
                            const char* label, const ImageFormat imageFormat)
      : start(startPtr), end(endPtr), name(label), format(imageFormat) {}
};

struct InputState {
  bool left = false;
  bool right = false;
  bool action = false;
  bool pause = false;
  bool trackPrev = false;
  bool trackNext = false;
  bool options = false;
  bool help = false;
  bool menuPrev = false;
  bool menuNext = false;
  bool leftPressed = false;
  bool rightPressed = false;
  bool actionPressed = false;
  bool pausePressed = false;
  bool trackPrevPressed = false;
  bool trackNextPressed = false;
  bool optionsPressed = false;
  bool helpPressed = false;
  bool menuPrevPressed = false;
  bool menuNextPressed = false;
  bool upPressed = false;
  bool downPressed = false;
  bool konamiBPressed = false;
  char typedChar = 0;
  float motionTurn = 0.0f;
  bool motionActionPressed = false;
};

struct ToneEvent {
  uint16_t frequency = 0;
  uint16_t duration = 0;
  uint16_t gap = 0;
};

M5Canvas* canvasPtr = nullptr;
M5Canvas* backgroundCanvasPtr = nullptr;
#define canvas (*canvasPtr)
#define backgroundCanvas (*backgroundCanvasPtr)

GameState gameState = GameState::Title;
GameState returnState = GameState::Title;
GameConfig config;
std::array<ScoreEntry, kScoreCount> highScores{};

Paddle paddle;
std::array<Ball, kMaxBalls> balls{};
std::array<Brick, kMaxBricks> bricks{};
std::array<PowerUp, kMaxPowerUps> powerUps{};
std::array<LaserShot, kMaxLasers> lasers{};
std::array<Particle, kMaxParticles> particles{};
std::array<TrailPoint, kMaxTrail> trail{};
std::array<uint8_t, kFireBufferSize> fireBuffer{};
std::array<uint16_t, 256> firePalette{};
size_t trailHead = 0;

uint32_t score = 0;
uint8_t lives = 3;
uint8_t level = 1;
uint8_t optionsIndex = 0;
uint8_t breakableLeft = 0;

bool sdReady = false;
bool prevLeft = false;
bool prevRight = false;
bool prevAction = false;
bool prevPause = false;
bool prevTrackPrev = false;
bool prevTrackNext = false;
bool prevOptions = false;
bool prevHelp = false;
bool prevMenuPrev = false;
bool prevMenuNext = false;
bool scoreStoredThisRound = false;

uint32_t lastFrameMs = 0;
uint32_t stateTimerMs = 0;
uint32_t laserUntilMs = 0;
uint32_t expandUntilMs = 0;
uint32_t slowUntilMs = 0;
uint32_t laserCooldownMs = 0;
uint32_t titleAnimMs = 0;
uint32_t uiInputUnlockMs = 0;
uint8_t activeBackgroundIndex = 0;
uint8_t cachedBackgroundIndex = 255;

std::array<ToneEvent, 16> toneQueue{};
size_t toneHead = 0;
size_t toneTail = 0;
uint32_t nextToneMs = 0;
bool waitingForBinding = false;
int8_t pendingHighScoreIndex = -1;
uint8_t nameEntryIndex = 0;
std::array<char, 3> nameEntryChars = {'A', 'A', 'A'};
std::vector<String> availableTracks;
std::vector<String> playlistDraft;
int playlistCursor = 0;
uint8_t konamiProgress = 0;
bool gyroControlsEnabled = false;
float filteredGyroZ = 0.0f;
float filteredAccelMagnitude = 1.0f;
uint32_t motionLaunchCooldownUntilMs = 0;

extern const uint8_t title_png_start[] asm("_binary_Gemini_240x128_png_start");
extern const uint8_t title_png_end[] asm("_binary_Gemini_240x128_png_end");
extern const uint8_t bg_1_png_start[] asm("_binary_sfondi_240_1_png_start");
extern const uint8_t bg_1_png_end[] asm("_binary_sfondi_240_1_png_end");
extern const uint8_t bg_2_png_start[] asm("_binary_sfondi_240_2_png_start");
extern const uint8_t bg_2_png_end[] asm("_binary_sfondi_240_2_png_end");
extern const uint8_t bg_3_png_start[] asm("_binary_sfondi_240_3_png_start");
extern const uint8_t bg_3_png_end[] asm("_binary_sfondi_240_3_png_end");
extern const uint8_t bg_4_png_start[] asm("_binary_sfondi_240_4_png_start");
extern const uint8_t bg_4_png_end[] asm("_binary_sfondi_240_4_png_end");
extern const uint8_t bg_5_jpg_start[] asm("_binary_sfondi_240_5_jpg_start");
extern const uint8_t bg_5_jpg_end[] asm("_binary_sfondi_240_5_jpg_end");
extern const uint8_t bg_6_jpg_start[] asm("_binary_sfondi_240_6_jpg_start");
extern const uint8_t bg_6_jpg_end[] asm("_binary_sfondi_240_6_jpg_end");
extern const uint8_t bg_7_jpg_start[] asm("_binary_sfondi_240_7_jpg_start");
extern const uint8_t bg_7_jpg_end[] asm("_binary_sfondi_240_7_jpg_end");
extern const uint8_t bg_8_jpg_start[] asm("_binary_sfondi_240_8_jpg_start");
extern const uint8_t bg_8_jpg_end[] asm("_binary_sfondi_240_8_jpg_end");
extern const uint8_t bg_9_jpg_start[] asm("_binary_sfondi_240_9_jpg_start");
extern const uint8_t bg_9_jpg_end[] asm("_binary_sfondi_240_9_jpg_end");
extern const uint8_t bg_10_jpg_start[] asm("_binary_sfondi_240_10_jpg_start");
extern const uint8_t bg_10_jpg_end[] asm("_binary_sfondi_240_10_jpg_end");
extern const uint8_t bg_11_png_start[] asm("_binary_sfondi_240_11_png_start");
extern const uint8_t bg_11_png_end[] asm("_binary_sfondi_240_11_png_end");
extern const uint8_t bg_12_png_start[] asm("_binary_sfondi_240_12_png_start");
extern const uint8_t bg_12_png_end[] asm("_binary_sfondi_240_12_png_end");
extern const uint8_t konami_1_jpg_start[] asm("_binary_sfondi_240_konami1_jpg_start");
extern const uint8_t konami_1_jpg_end[] asm("_binary_sfondi_240_konami1_jpg_end");
extern const uint8_t konami_2_jpg_start[] asm("_binary_sfondi_240_konami2_jpg_start");
extern const uint8_t konami_2_jpg_end[] asm("_binary_sfondi_240_konami2_jpg_end");

const std::array<BackgroundAsset, kBackgroundCount> kBackgroundAssets = {{
    {bg_1_png_start, bg_1_png_end, "1.PNG", ImageFormat::Png},
    {bg_2_png_start, bg_2_png_end, "2.PNG", ImageFormat::Png},
    {bg_3_png_start, bg_3_png_end, "3.PNG", ImageFormat::Png},
    {bg_4_png_start, bg_4_png_end, "4.PNG", ImageFormat::Png},
    {bg_5_jpg_start, bg_5_jpg_end, "5.JPG", ImageFormat::Jpg},
    {bg_6_jpg_start, bg_6_jpg_end, "6.JPG", ImageFormat::Jpg},
    {bg_7_jpg_start, bg_7_jpg_end, "7.JPG", ImageFormat::Jpg},
    {bg_8_jpg_start, bg_8_jpg_end, "8.JPG", ImageFormat::Jpg},
    {bg_9_jpg_start, bg_9_jpg_end, "9.JPG", ImageFormat::Jpg},
    {bg_10_jpg_start, bg_10_jpg_end, "10.JPG", ImageFormat::Jpg},
    {bg_11_png_start, bg_11_png_end, "11.PNG", ImageFormat::Png},
    {bg_12_png_start, bg_12_png_end, "12.PNG", ImageFormat::Png},
}};

const std::array<KonamiToken, kKonamiCodeLength> kKonamiCode = {{
    KonamiToken::Up,    KonamiToken::Up,   KonamiToken::Down, KonamiToken::Down,
    KonamiToken::Left,  KonamiToken::Right, KonamiToken::Left, KonamiToken::Right,
    KonamiToken::B,     KonamiToken::A,    KonamiToken::Enter,
}};

float clampf(const float value, const float low, const float high) {
  return std::max(low, std::min(high, value));
}

float lerpf(const float a, const float b, const float t) {
  return a + (b - a) * t;
}

uint16_t rgb565(const uint8_t r, const uint8_t g, const uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t brickColorForHits(const uint8_t hits, const uint8_t variant) {
  const uint8_t index = variant & 0x07;
  switch (hits) {
    case 3: {
      static const uint16_t palette[] = {
          rgb565(180, 108, 255), rgb565(140, 118, 255), rgb565(198, 98, 242),
          rgb565(122, 136, 255), rgb565(172, 126, 236), rgb565(150, 102, 255),
          rgb565(196, 132, 255), rgb565(132, 96, 232),
      };
      return palette[index];
    }
    case 2: {
      static const uint16_t palette[] = {
          rgb565(242, 244, 255), rgb565(255, 255, 255), rgb565(232, 246, 255),
          rgb565(248, 250, 242), rgb565(236, 238, 246), rgb565(226, 242, 255),
          rgb565(244, 244, 232), rgb565(230, 230, 238),
      };
      return palette[index];
    }
    default: {
      static const uint16_t palette[] = {
          rgb565(255, 88, 96), rgb565(255, 124, 78), rgb565(255, 96, 62),
          rgb565(255, 148, 62), rgb565(236, 74, 88), rgb565(255, 170, 78),
          rgb565(228, 98, 54), rgb565(255, 116, 116),
      };
      return palette[index];
    }
  }
}

uint16_t mix565(const uint16_t a, const uint16_t b, const float t) {
  const float clamped = clampf(t, 0.0f, 1.0f);
  const uint8_t ar = static_cast<uint8_t>(((a >> 11) & 0x1F) * 255 / 31);
  const uint8_t ag = static_cast<uint8_t>(((a >> 5) & 0x3F) * 255 / 63);
  const uint8_t ab = static_cast<uint8_t>((a & 0x1F) * 255 / 31);
  const uint8_t br = static_cast<uint8_t>(((b >> 11) & 0x1F) * 255 / 31);
  const uint8_t bg = static_cast<uint8_t>(((b >> 5) & 0x3F) * 255 / 63);
  const uint8_t bb = static_cast<uint8_t>((b & 0x1F) * 255 / 31);
  return rgb565(
      static_cast<uint8_t>(lerpf(ar, br, clamped)),
      static_cast<uint8_t>(lerpf(ag, bg, clamped)),
      static_cast<uint8_t>(lerpf(ab, bb, clamped)));
}

void initFireBackdrop() {
  for (int v = 0; v < 256; ++v) {
    const uint8_t r = (v * 2 > 255) ? 255 : (v * 2);
    const uint8_t g = (v > 128) ? (v - 128) * 2 : 0;
    const uint8_t b = (v > 192) ? (v - 192) * 4 : 0;
    firePalette[static_cast<size_t>(v)] = rgb565(r, g, b);
  }
  fireBuffer.fill(0);
}

void resetFireBackdrop() {
  fireBuffer.fill(0);
}

bool wordHas(const Keyboard_Class::KeysState& state, const char target) {
  for (const auto c : state.word) {
    if (c == target) {
      return true;
    }
  }
  return false;
}

bool hidHas(const Keyboard_Class::KeysState& state, const uint8_t hid) {
  for (const auto code : state.hid_keys) {
    if (code == hid) {
      return true;
    }
  }
  return false;
}

char normalizeBindingChar(const char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool normalizedWordHas(const Keyboard_Class::KeysState& state, const char target) {
  const char normalizedTarget = normalizeBindingChar(target);
  for (const auto c : state.word) {
    if (normalizeBindingChar(c) == normalizedTarget) {
      return true;
    }
  }
  return false;
}

char firstPrintableNormalized(const Keyboard_Class::KeysState& state) {
  for (const auto c : state.word) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 32 && uc <= 126) {
      return normalizeBindingChar(c);
    }
  }
  return 0;
}

bool isReservedBinding(const char c) {
  const char normalized = normalizeBindingChar(c);
  return normalized == '`' || normalized == 'v';
}

bool isDuplicateBinding(const char c, const uint8_t ignoreIndex) {
  const char normalized = normalizeBindingChar(c);
  if (ignoreIndex != 6 && normalized == normalizeBindingChar(config.leftKey)) {
    return true;
  }
  if (ignoreIndex != 7 && normalized == normalizeBindingChar(config.rightKey)) {
    return true;
  }
  if (ignoreIndex != 8 && normalized == normalizeBindingChar(config.actionKey)) {
    return true;
  }
  return false;
}

char* optionBindingRef(const uint8_t index) {
  switch (index) {
    case 6:
      return &config.leftKey;
    case 7:
      return &config.rightKey;
    case 8:
      return &config.actionKey;
    default:
      return nullptr;
  }
}

const char* bindingLabel(const char key) {
  static char labels[3][8];
  static uint8_t next = 0;
  next = (next + 1) % 3;

  char* buffer = labels[next];
  const char normalized = normalizeBindingChar(key);
  if (normalized == ' ') {
    std::snprintf(buffer, 8, "SPACE");
  } else {
    buffer[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized)));
    buffer[1] = '\0';
  }
  return buffer;
}

int nameCharIndex(const char c) {
  const char normalized = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (size_t i = 0; i < sizeof(kNameCharset) - 1; ++i) {
    if (kNameCharset[i] == normalized) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

char stepNameChar(const char current, const int delta) {
  const int count = static_cast<int>(sizeof(kNameCharset) - 1);
  int index = nameCharIndex(current);
  index = (index + delta + count) % count;
  return kNameCharset[index];
}

char displayNameChar(const char c) {
  return c == ' ' ? '_' : c;
}

size_t titlePngSize() {
  return static_cast<size_t>(title_png_end - title_png_start);
}

size_t binaryAssetSize(const uint8_t* start, const uint8_t* end) {
  return static_cast<size_t>(end - start);
}

void resetGyroMotionState() {
  filteredGyroZ = 0.0f;
  filteredAccelMagnitude = 1.0f;
  motionLaunchCooldownUntilMs = 0;
}

void setGyroControlsEnabled(const bool enabled) {
  gyroControlsEnabled = enabled;
  resetGyroMotionState();
}

uint8_t clampBackgroundIndex(const int index) {
  if (index < 0) {
    return 0;
  }
  if (index >= static_cast<int>(kBackgroundAssets.size())) {
    return static_cast<uint8_t>(kBackgroundAssets.size() - 1);
  }
  return static_cast<uint8_t>(index);
}

uint8_t requestedBackgroundIndex() {
  if (gameState == GameState::BackgroundSelect) {
    return clampBackgroundIndex(config.backgroundIndex);
  }
  return clampBackgroundIndex(activeBackgroundIndex);
}

const BackgroundAsset& currentBackground() {
  return kBackgroundAssets[requestedBackgroundIndex()];
}

void invalidateBackgroundCache() {
  cachedBackgroundIndex = 255;
}

bool lockSD(const TickType_t timeout = portMAX_DELAY) {
  return sdMutex == nullptr || xSemaphoreTake(sdMutex, timeout);
}

void unlockSD() {
  if (sdMutex != nullptr) {
    xSemaphoreGive(sdMutex);
  }
}

void releaseLegacyI2S() {
  i2s_driver_uninstall(I2S_NUM_0);
#if SOC_I2S_NUM > 1
  i2s_driver_uninstall(I2S_NUM_1);
#endif
}

uint8_t mp3VolumeValue() {
  return static_cast<uint8_t>(config.mp3VolumeStep * 25);
}

uint8_t sfxVolumeValue() {
  return static_cast<uint8_t>(config.volumeStep * 25);
}

String trimTrackName(const String& track) {
  const int slash = track.lastIndexOf('/');
  const String base = slash >= 0 ? track.substring(slash + 1) : track;
  if (base.length() <= 18) {
    return base;
  }
  return base.substring(0, 15) + "...";
}

void drawScrollingText(const String& text, const int x, const int y, const int width,
                       const uint16_t textColor, const uint16_t bgColor,
                       const uint32_t phaseSeed = 0) {
  canvas.fillRect(x, y - 1, width, canvas.fontHeight() + 2, bgColor);
  canvas.setTextColor(textColor, bgColor);

  const int textWidth = canvas.textWidth(text);
  if (textWidth <= width) {
    canvas.drawString(text, x, y);
    return;
  }

  const int gap = 18;
  const uint32_t ticks = (millis() + phaseSeed) / 120;
  const int cycle = textWidth + gap;
  const int offset = static_cast<int>(ticks % cycle);

  canvas.setClipRect(x, y - 1, width, canvas.fontHeight() + 2);
  canvas.drawString(text, x - offset, y);
  canvas.drawString(text, x - offset + cycle, y);
  canvas.clearClipRect();
}

bool playlistContains(const String& track) {
  for (const auto& item : playlistDraft) {
    if (item == track) {
      return true;
    }
  }
  return false;
}

void togglePlaylistTrack(const String& track) {
  for (size_t i = 0; i < playlistDraft.size(); ++i) {
    if (playlistDraft[i] == track) {
      playlistDraft.erase(playlistDraft.begin() + i);
      return;
    }
  }
  playlistDraft.push_back(track);
}

void pushTrail(const float x, const float y) {
  trail[trailHead].x = x;
  trail[trailHead].y = y;
  trail[trailHead].active = true;
  trailHead = (trailHead + 1) % trail.size();
}

void clearTrail() {
  for (auto& point : trail) {
    point.active = false;
  }
  trailHead = 0;
}

void clearParticles() {
  for (auto& particle : particles) {
    particle.active = false;
  }
}

void applyAudioConfig() {
  M5Cardputer.Speaker.setVolume(192);
  M5Cardputer.Speaker.setChannelVolume(kSfxVirtualChannel, sfxVolumeValue());
  M5Cardputer.Speaker.setChannelVolume(kMp3VirtualChannel, 255);
  AudioTask::setMP3Volume(mp3VolumeValue());
  AudioTask::mp3Enabled = config.mp3Enabled;
  if (!config.sfx) {
    toneHead = 0;
    toneTail = 0;
    nextToneMs = 0;
  }
}

void queueTone(const uint16_t frequency, const uint16_t duration,
               const uint16_t gap = 18) {
  if (!config.sfx || frequency == 0 || duration == 0) {
    return;
  }

  const size_t nextTail = (toneTail + 1) % toneQueue.size();
  if (nextTail == toneHead) {
    return;
  }

  toneQueue[toneTail].frequency = frequency;
  toneQueue[toneTail].duration = duration;
  toneQueue[toneTail].gap = gap;
  toneTail = nextTail;
}

void updateAudio() {
  if (!config.sfx) {
    return;
  }

  const uint32_t now = millis();
  if (now < nextToneMs || toneHead == toneTail) {
    return;
  }

  const ToneEvent tone = toneQueue[toneHead];
  toneHead = (toneHead + 1) % toneQueue.size();
  M5Cardputer.Speaker.tone(tone.frequency, tone.duration, kSfxVirtualChannel, true);
  nextToneMs = now + tone.duration + tone.gap;
}

void sfxPaddle() {
  queueTone(1650, 20, 6);
}

void sfxBrick() {
  queueTone(4200, 12, 4);
}

void sfxLaser() {
  queueTone(6200, 12, 2);
}

void sfxPower() {
  queueTone(1800, 24, 6);
  queueTone(2400, 30, 6);
  queueTone(3200, 36, 12);
}

void sfxLoseLife() {
  queueTone(1400, 80, 10);
  queueTone(1100, 90, 10);
  queueTone(820, 120, 18);
}

void sfxLevelClear() {
  queueTone(1500, 30, 6);
  queueTone(1900, 30, 6);
  queueTone(2500, 34, 6);
  queueTone(3200, 52, 20);
}

void sfxGameOver() {
  queueTone(900, 90, 12);
  queueTone(760, 90, 12);
  queueTone(620, 120, 20);
}

void sfxStart() {
  queueTone(1800, 18, 4);
  queueTone(2600, 18, 4);
  queueTone(3400, 28, 12);
}

void sfxKonamiUnlock() {
  queueTone(1320, 70, 10);
  queueTone(1760, 70, 10);
  queueTone(2200, 80, 12);
  queueTone(2640, 80, 12);
  queueTone(3520, 120, 18);
}

void sfxKonamiDisable() {
  queueTone(1800, 44, 8);
  queueTone(1320, 56, 8);
  queueTone(900, 72, 16);
}

void resetKonamiProgress() {
  konamiProgress = 0;
}

bool nextKonamiToken(const InputState& input, KonamiToken& token) {
  if (input.upPressed) {
    token = KonamiToken::Up;
    return true;
  }
  if (input.downPressed) {
    token = KonamiToken::Down;
    return true;
  }
  if (input.leftPressed) {
    token = KonamiToken::Left;
    return true;
  }
  if (input.rightPressed) {
    token = KonamiToken::Right;
    return true;
  }
  if (input.konamiBPressed) {
    token = KonamiToken::B;
    return true;
  }
  if (input.actionPressed) {
    token = KonamiToken::A;
    return true;
  }
  if (input.pausePressed) {
    token = KonamiToken::Enter;
    return true;
  }
  return false;
}

void updateMotionInput(InputState& input) {
  if (!gyroControlsEnabled || !M5.Imu.isEnabled()) {
    return;
  }

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);

  filteredGyroZ = lerpf(filteredGyroZ, gz * kGyroTurnSign, 0.28f);
  const float gyroAbs = std::fabs(filteredGyroZ);
  if (gyroAbs > kGyroTurnDeadzone) {
    const float scaled =
        clampf((gyroAbs - kGyroTurnDeadzone) / (kGyroTurnFullScale - kGyroTurnDeadzone), 0.0f, 1.0f);
    const float accelerated = clampf(
        scaled + (scaled * scaled * kGyroAccelerationStrength), 0.0f, 1.0f);
    input.motionTurn = clampf((filteredGyroZ > 0.0f ? 1.0f : -1.0f) * accelerated *
                                  kGyroSensitivityBoost,
                              -1.0f, 1.0f);
  }

  const float accelMagnitude = std::sqrt(ax * ax + ay * ay + az * az);
  filteredAccelMagnitude =
      lerpf(filteredAccelMagnitude, accelMagnitude, kMotionFilterBlend);
  const float accelImpulse = accelMagnitude - filteredAccelMagnitude;
  const uint32_t now = millis();
  if (accelImpulse > kMotionLaunchThreshold && now >= motionLaunchCooldownUntilMs) {
    input.motionActionPressed = true;
    motionLaunchCooldownUntilMs = now + kMotionLaunchCooldownMs;
  }
}

void beginKonamiReveal() {
  resetKonamiProgress();
  gameState = GameState::KonamiReveal;
  stateTimerMs = millis();
  sfxKonamiUnlock();
}

void beginGyroOffScreen() {
  resetKonamiProgress();
  gameState = GameState::GyroOff;
  stateTimerMs = millis();
  sfxKonamiDisable();
}

bool handleKonamiCodeOnTitle(const InputState& input) {
  KonamiToken token{};
  if (!nextKonamiToken(input, token)) {
    return false;
  }

  const bool hadProgress = konamiProgress != 0;
  if (token == kKonamiCode[konamiProgress]) {
    ++konamiProgress;
    if (konamiProgress >= kKonamiCode.size()) {
      resetKonamiProgress();
      if (gyroControlsEnabled) {
        setGyroControlsEnabled(false);
        beginGyroOffScreen();
      } else {
        beginKonamiReveal();
      }
    }
    return true;
  }

  konamiProgress = (token == kKonamiCode[0]) ? 1 : 0;
  return hadProgress || konamiProgress != 0;
}

void sanitizeBindings();
void saveHighScores();
void saveConfig();
void syncAudioPlayback(bool forceRestart = false);

void setDefaultHighScores() {
  const char* defaultNames[kScoreCount] = {"ADV", "GLS", "NEO", "ARK", "PIX"};
  const uint32_t defaultScores[kScoreCount] = {3000, 2200, 1600, 1100, 800};
  for (size_t i = 0; i < kScoreCount; ++i) {
    std::snprintf(highScores[i].name, sizeof(highScores[i].name), "%s", defaultNames[i]);
    highScores[i].score = defaultScores[i];
  }
}

void loadConfig() {
  config = {};
  if (!sdReady || !SD.exists(kConfigFile)) {
    return;
  }
  if (!lockSD()) {
    return;
  }

  File file = SD.open(kConfigFile, FILE_READ);
  if (!file) {
    unlockSD();
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    const int equal = line.indexOf('=');
    if (equal < 0) {
      continue;
    }

    const String key = line.substring(0, equal);
    const String value = line.substring(equal + 1);
    if (key == "sfx") {
      config.sfx = value.toInt() != 0;
    } else if (key == "volume") {
      config.volumeStep = static_cast<uint8_t>(clampf(value.toInt(), 0, 10));
    } else if (key == "mp3") {
      config.mp3Enabled = value.toInt() != 0;
    } else if (key == "mp3Volume") {
      config.mp3VolumeStep = static_cast<uint8_t>(clampf(value.toInt(), 0, 10));
    } else if (key == "difficulty") {
      config.difficulty = static_cast<uint8_t>(clampf(value.toInt(), 0, 2));
    } else if (key == "trails") {
      config.trails = value.toInt() != 0;
    } else if (key == "paddle") {
      config.paddleSize = static_cast<uint8_t>(clampf(value.toInt(), 0, 2));
    } else if (key == "backgroundMode") {
      config.randomBackground = value.toInt() != 0;
    } else if (key == "background") {
      config.backgroundIndex = clampBackgroundIndex(value.toInt());
    } else if (key == "leftKey" && !value.isEmpty()) {
      config.leftKey = normalizeBindingChar(value[0]);
    } else if (key == "rightKey" && !value.isEmpty()) {
      config.rightKey = normalizeBindingChar(value[0]);
    } else if (key == "actionKey" && !value.isEmpty()) {
      config.actionKey = normalizeBindingChar(value[0]);
    }
  }
  file.close();
  unlockSD();
  sanitizeBindings();
}

void saveConfig() {
  if (!sdReady) {
    return;
  }

  if (!SD.exists(kDataDir)) {
    SD.mkdir(kDataDir);
  }
  if (!lockSD()) {
    return;
  }
  if (SD.exists(kConfigFile)) {
    SD.remove(kConfigFile);
  }

  File file = SD.open(kConfigFile, FILE_WRITE);
  if (!file) {
    unlockSD();
    return;
  }

  file.print("sfx=");
  file.println(config.sfx ? 1 : 0);
  file.print("volume=");
  file.println(config.volumeStep);
  file.print("mp3=");
  file.println(config.mp3Enabled ? 1 : 0);
  file.print("mp3Volume=");
  file.println(config.mp3VolumeStep);
  file.print("difficulty=");
  file.println(config.difficulty);
  file.print("trails=");
  file.println(config.trails ? 1 : 0);
  file.print("paddle=");
  file.println(config.paddleSize);
  file.print("backgroundMode=");
  file.println(config.randomBackground ? 1 : 0);
  file.print("background=");
  file.println(config.backgroundIndex);
  file.print("leftKey=");
  file.println(config.leftKey);
  file.print("rightKey=");
  file.println(config.rightKey);
  file.print("actionKey=");
  file.println(config.actionKey);
  file.close();
  unlockSD();
}

void loadHighScores() {
  setDefaultHighScores();
  if (!sdReady || !SD.exists(kScoreFile)) {
    return;
  }
  if (!lockSD()) {
    return;
  }

  File file = SD.open(kScoreFile, FILE_READ);
  if (!file) {
    unlockSD();
    return;
  }

  size_t index = 0;
  while (file.available() && index < highScores.size()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) {
      continue;
    }

    const int split = line.indexOf(' ');
    if (split < 1) {
      continue;
    }

    String name = line.substring(0, split);
    name.trim();
    name.toUpperCase();
    if (name.length() > 3) {
      name = name.substring(0, 3);
    }

    ScoreEntry entry{};
    entry.name[0] = name.length() > 0 ? name[0] : '-';
    entry.name[1] = name.length() > 1 ? name[1] : '-';
    entry.name[2] = name.length() > 2 ? name[2] : '-';
    entry.name[3] = '\0';
    entry.score = static_cast<uint32_t>(line.substring(split + 1).toInt());
    highScores[index++] = entry;
  }
  file.close();
  unlockSD();
}

void saveHighScores() {
  if (!sdReady) {
    return;
  }

  if (!SD.exists(kDataDir)) {
    SD.mkdir(kDataDir);
  }
  if (!lockSD()) {
    return;
  }
  if (SD.exists(kScoreFile)) {
    SD.remove(kScoreFile);
  }

  File file = SD.open(kScoreFile, FILE_WRITE);
  if (!file) {
    unlockSD();
    return;
  }

  for (const auto& entry : highScores) {
    file.print(entry.name);
    file.print(' ');
    file.println(entry.score);
  }
  file.close();
  unlockSD();
}

int findHighScoreInsertIndex(const uint32_t newScore) {
  for (size_t i = 0; i < highScores.size(); ++i) {
    if (newScore > highScores[i].score) {
      return static_cast<int>(i);
    }
  }

  if (newScore > highScores.back().score) {
    return static_cast<int>(highScores.size() - 1);
  }
  return -1;
}

void insertHighScore(const ScoreEntry& entry, const int index) {
  if (index < 0 || index >= static_cast<int>(highScores.size())) {
    return;
  }

  for (size_t j = highScores.size() - 1; j > static_cast<size_t>(index); --j) {
    highScores[j] = highScores[j - 1];
  }
  highScores[static_cast<size_t>(index)] = entry;
  saveHighScores();
}

void beginNameEntry(const uint32_t newScore) {
  pendingHighScoreIndex = static_cast<int8_t>(findHighScoreInsertIndex(newScore));
  if (pendingHighScoreIndex < 0) {
    gameState = GameState::GameOver;
    stateTimerMs = millis();
    return;
  }

  nameEntryIndex = 0;
  nameEntryChars = {'A', 'A', 'A'};
  scoreStoredThisRound = false;
  resetFireBackdrop();
  gameState = GameState::NameEntry;
  stateTimerMs = millis();
}

void finalizeNameEntry() {
  ScoreEntry entry{};
  entry.name[0] = nameEntryChars[0];
  entry.name[1] = nameEntryChars[1];
  entry.name[2] = nameEntryChars[2];
  entry.name[3] = '\0';
  entry.score = score;
  insertHighScore(entry, pendingHighScoreIndex);
  pendingHighScoreIndex = -1;
  scoreStoredThisRound = true;
  gameState = GameState::GameOver;
  stateTimerMs = millis();
}

bool initStorage() {
  SPI.begin(GPIO_NUM_40, GPIO_NUM_39, GPIO_NUM_14, GPIO_NUM_12);
  sdReady = SD.begin(GPIO_NUM_12, SPI, 25000000);
  if (sdReady) {
    if (!SD.exists(kDataDir)) {
      SD.mkdir(kDataDir);
    }
    if (!SD.exists("/ADVnoid/music")) {
      SD.mkdir("/ADVnoid/music");
    }
    if (!SD.exists("/ADVnoid/playlist.txt")) {
      File playlistFile = SD.open("/ADVnoid/playlist.txt", FILE_WRITE);
      if (playlistFile) {
        playlistFile.close();
      }
    }
  }
  return sdReady;
}

void sanitizeBindings() {
  config.mp3VolumeStep = static_cast<uint8_t>(clampf(config.mp3VolumeStep, 0, 10));
  config.backgroundIndex = clampBackgroundIndex(config.backgroundIndex);
  config.leftKey = normalizeBindingChar(config.leftKey);
  config.rightKey = normalizeBindingChar(config.rightKey);
  config.actionKey = normalizeBindingChar(config.actionKey);

  if (config.leftKey == 0 || isReservedBinding(config.leftKey)) {
    config.leftKey = 'a';
  }
  if (config.rightKey == 0 || isReservedBinding(config.rightKey) ||
      config.rightKey == config.leftKey) {
    config.rightKey = 'd';
  }
  if (config.actionKey == 0 || isReservedBinding(config.actionKey) ||
      config.actionKey == config.leftKey || config.actionKey == config.rightKey) {
    config.actionKey = 'p';
  }
}

void selectGameplayBackground() {
  if (config.randomBackground) {
    uint8_t next = activeBackgroundIndex;
    if (kBackgroundAssets.size() > 1) {
      while (next == activeBackgroundIndex) {
        next = static_cast<uint8_t>(random(static_cast<int>(kBackgroundAssets.size())));
      }
    } else {
      next = 0;
    }
    activeBackgroundIndex = next;
  } else {
    activeBackgroundIndex = clampBackgroundIndex(config.backgroundIndex);
  }
  invalidateBackgroundCache();
}

void refreshBackgroundCache() {
  if (backgroundCanvasPtr == nullptr) {
    return;
  }
  const uint8_t index = requestedBackgroundIndex();
  if (cachedBackgroundIndex == index) {
    return;
  }

  const auto& bg = currentBackground();
  backgroundCanvas.fillScreen(rgb565(8, 12, 26));
  const size_t size = static_cast<size_t>(bg.end - bg.start);
  if (size > 0) {
    if (bg.format == ImageFormat::Png) {
      backgroundCanvas.drawPng(bg.start, size, 0, 0);
    } else {
      backgroundCanvas.drawJpg(bg.start, size, 0, 0);
    }
  }
  cachedBackgroundIndex = index;
}

float difficultyBallScale() {
  if (config.difficulty == 0) {
    return 0.93f;
  }
  if (config.difficulty == 2) {
    return 1.12f;
  }
  return 1.0f;
}

float difficultyPaddleScale() {
  if (config.difficulty == 0) {
    return 1.10f;
  }
  if (config.difficulty == 2) {
    return 0.92f;
  }
  return 1.0f;
}

float basePaddleWidth() {
  if (config.paddleSize == 0) {
    return 32.0f;
  }
  if (config.paddleSize == 2) {
    return 44.0f;
  }
  return 38.0f;
}

float currentPaddleWidth() {
  float width = basePaddleWidth();
  if (millis() < expandUntilMs) {
    width += 14.0f;
  }
  return width;
}

float currentBallSpeed() {
  const float levelBoost = 88.0f + static_cast<float>(level) * 7.0f;
  const float slowMul = millis() < slowUntilMs ? 0.82f : 1.0f;
  return levelBoost * difficultyBallScale() * slowMul;
}

float currentPaddleSpeed() {
  return 164.0f * difficultyPaddleScale();
}

size_t activeBallCount() {
  size_t count = 0;
  for (const auto& currentBall : balls) {
    if (currentBall.active) {
      ++count;
    }
  }
  return count;
}

bool hasLaunchedBall() {
  for (const auto& currentBall : balls) {
    if (currentBall.active && !currentBall.stuck) {
      return true;
    }
  }
  return false;
}

Ball* firstActiveBall() {
  for (auto& currentBall : balls) {
    if (currentBall.active) {
      return &currentBall;
    }
  }
  return nullptr;
}

Ball* firstStuckBall() {
  for (auto& currentBall : balls) {
    if (currentBall.active && currentBall.stuck) {
      return &currentBall;
    }
  }
  return nullptr;
}

int trailBallIndex() {
  for (size_t i = 0; i < balls.size(); ++i) {
    if (balls[i].active && !balls[i].stuck) {
      return static_cast<int>(i);
    }
  }
  for (size_t i = 0; i < balls.size(); ++i) {
    if (balls[i].active) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void normalizeBallSpeed(Ball& ball) {
  const float target = currentBallSpeed();
  float len = std::sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
  if (len < 0.0001f) {
    ball.vx = target * 0.35f;
    ball.vy = -target * 0.94f;
    len = std::sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
  }

  ball.vx = (ball.vx / len) * target;
  ball.vy = (ball.vy / len) * target;

  const float minHorizontal = target * 0.22f;
  if (std::fabs(ball.vx) < minHorizontal) {
    ball.vx = (ball.vx < 0.0f ? -1.0f : 1.0f) * minHorizontal;
    const float verticalSign = ball.vy < 0.0f ? -1.0f : 1.0f;
    const float verticalAbs =
        std::sqrt(std::max(1.0f, target * target - ball.vx * ball.vx));
    ball.vy = verticalSign * verticalAbs;
  }
}

void resetBallsOnPaddle() {
  paddle.w = currentPaddleWidth();
  for (auto& ball : balls) {
    ball = {};
  }

  Ball& ball = balls[0];
  ball.active = true;
  ball.stuck = true;
  ball.r = 2.8f;
  ball.x = paddle.x + paddle.w * 0.5f;
  ball.y = kPaddleY - ball.r - 1.0f;
  ball.vx = currentBallSpeed() * 0.38f;
  ball.vy = -currentBallSpeed();
  clearTrail();
}

void clearEntities() {
  for (auto& power : powerUps) {
    power.active = false;
  }
  for (auto& shot : lasers) {
    shot.active = false;
  }
  clearParticles();
  clearTrail();
  laserUntilMs = 0;
  expandUntilMs = 0;
  slowUntilMs = 0;
  laserCooldownMs = 0;
}

void spawnLevelBrick(const int row, const int col, const char cell, size_t& index) {
  if (cell == '.' || index >= bricks.size()) {
    return;
  }

  constexpr float brickW = 22.0f;
  constexpr float brickH = 10.0f;
  constexpr float gap = 0.0f;
  constexpr float top = 19.0f;

  Brick brick{};
  brick.x = 10.0f + col * (brickW + gap);
  brick.baseX = brick.x;
  brick.y = top + row * (brickH + 4.0f);
  brick.w = brickW;
  brick.h = brickH;
  brick.alive = true;
  brick.breakable = cell != 'W' && cell != 'M';
  brick.colorVariant = static_cast<uint8_t>((row * 11 + col * 7 + level * 5) & 0x07);
  if (cell == 'W') {
    brick.x += 1.0f;
    brick.baseX = brick.x;
    brick.w -= 2.0f;
    brick.hits = 0;
    brick.color = rgb565(156, 170, 196);
  } else if (cell == 'M') {
    brick.x += 1.0f;
    brick.baseX = brick.x;
    brick.w -= 2.0f;
    brick.hits = 0;
    brick.color = rgb565(104, 220, 255);
    brick.motionRange = 5.0f + static_cast<float>((row + col + level) % 3);
    brick.motionSpeed = 1.1f + static_cast<float>((level + row) % 4) * 0.16f;
    brick.motionPhase = static_cast<float>((row * 37 + col * 19 + level * 11) % 360) *
                        0.0174532925f;
  } else {
    brick.hits = cell == 'T' ? 3 : (cell == 'H' ? 2 : 1);
    brick.color = brickColorForHits(brick.hits, brick.colorVariant);
    ++breakableLeft;
  }

  bricks[index++] = brick;
}

char resolveLevelCell(char cell, const int row, const int col) {
  const bool isWall = cell == 'W' || cell == 'M';
  if (level <= 2) {
    if (isWall) {
      return 'B';
    }
    return cell;
  }

  if (level <= 6) {
    if (cell == 'M') {
      return ((row + col + level) % 2 == 0) ? 'M' : 'B';
    }
    if (cell == 'W') {
      return ((row * 3 + col + level) % 3 == 0) ? 'W' : 'B';
    }
    return cell;
  }

  if (cell == '.' && (col == 2 || col == 7) && row >= 1) {
    return row == 2 ? 'M' : 'W';
  }

  if (cell == 'B' && (col == 2 || col == 7) && row >= 1) {
    return row == 2 ? 'M' : 'W';
  }

  return cell;
}

void generateLevel() {
  selectGameplayBackground();
  clearEntities();
  breakableLeft = 0;

  static constexpr std::array<std::array<const char*, 4>, 8> layouts = {{
      {{"BBBBBBBBBB", "B..W..W..B", "BHBBBBBBHB", "B.B....B.B"}},
      {{"BB.WWW.BBB", "BHB....BHB", "B.BBBB.B.B", "BBW....WBB"}},
      {{"BWBHBBHBWB", "BBBB..BBBB", "B..WBBW..B", "BHBBBBBBHB"}},
      {{"BBBHWHHBBB", "B..W..W..B", "BBBBBBBBBB", "BHW....WHB"}},
      {{"BBBM..MBBB", "B..WBBW..B", "BHBBBBBBHB", "BB.MWW.MBB"}},
      {{"BHBHBBHBHB", "BM......MB", "BB.WWW.BB.", "BHBHBBHBHB"}},
      {{"BBWMMWW.BB", "B..BBBB..B", "BH.W..W.HB", "BBB....BBB"}},
      {{"BMMBHHBMMB", "B..W..W..B", "BBBBBBBBBB", "BHMB..BMHB"}},
  }};

  size_t index = 0;
  const auto& layout = layouts[(level - 1) % layouts.size()];
  for (size_t row = 0; row < layout.size(); ++row) {
    for (int col = 0; layout[row][col] != '\0'; ++col) {
      char cell = resolveLevelCell(layout[row][col], static_cast<int>(row), col);
      if (cell == 'B') {
        const int roll = (static_cast<int>(row) * 7 + col * 3 + level) % 11;
        if (level >= 5 && roll == 0) {
          cell = 'T';
        } else if (level > 2 && (roll == 3 || roll == 7)) {
          cell = 'H';
        }
      }
      spawnLevelBrick(static_cast<int>(row), col, cell, index);
    }
  }

  for (; index < bricks.size(); ++index) {
    bricks[index].alive = false;
  }

  paddle.x = (kScreenW - currentPaddleWidth()) * 0.5f;
  paddle.w = currentPaddleWidth();
  resetBallsOnPaddle();
}

void newGame() {
  score = 0;
  lives = 3;
  level = 1;
  optionsIndex = 0;
  scoreStoredThisRound = false;
  pendingHighScoreIndex = -1;
  nameEntryIndex = 0;
  nameEntryChars = {'A', 'A', 'A'};
  resetKonamiProgress();
  generateLevel();
  gameState = GameState::Playing;
  stateTimerMs = millis();
  sfxStart();
  if (config.mp3Enabled && !AudioTask::isPlaying()) {
    syncAudioPlayback();
  }
}

void returnToTitle() {
  clearEntities();
  resetBallsOnPaddle();
  resetKonamiProgress();
  gameState = GameState::Title;
  stateTimerMs = millis();
  syncAudioPlayback();
}

void openBackgroundSelector(const GameState fromState) {
  returnState = fromState;
  gameState = GameState::BackgroundSelect;
  uiInputUnlockMs = millis() + 120;
}

void handleTrackShortcuts(const InputState& input) {
  if (!config.mp3Enabled || waitingForBinding || gameState == GameState::Playlist) {
    return;
  }

  if (input.trackPrevPressed) {
    AudioTask::prevTrack();
    queueTone(1350, 14, 4);
  } else if (input.trackNextPressed) {
    AudioTask::nextTrack();
    queueTone(1620, 14, 4);
  }
}

void spawnPowerUp(const float x, const float y) {
  const int roll = random(100);
  if (roll > 28) {
    return;
  }

  for (auto& power : powerUps) {
    if (!power.active) {
      power.active = true;
      power.x = x;
      power.y = y;
      power.vy = 42.0f + random(0, 12);
      if (roll < 8) {
        power.type = PowerType::Expand;
      } else if (roll < 15) {
        power.type = PowerType::Laser;
      } else if (roll < 22) {
        power.type = PowerType::Slow;
      } else {
        power.type = PowerType::MultiBall;
      }
      return;
    }
  }
}

void spawnBrickParticles(const Brick& brick, const uint16_t color, const uint8_t count) {
  const float cx = brick.x + brick.w * 0.5f;
  const float cy = brick.y + brick.h * 0.5f;
  for (uint8_t i = 0; i < count; ++i) {
    for (auto& particle : particles) {
      if (particle.active) {
        continue;
      }

      const float angle = static_cast<float>(random(0, 628)) / 100.0f;
      const float speed = 26.0f + static_cast<float>(random(0, 80));
      particle.active = true;
      particle.x = cx + static_cast<float>(random(-8, 9)) * 0.2f;
      particle.y = cy + static_cast<float>(random(-6, 7)) * 0.2f;
      particle.vx = std::cos(angle) * speed;
      particle.vy = std::sin(angle) * speed - 18.0f;
      particle.life = 0.34f + static_cast<float>(random(0, 18)) / 100.0f;
      particle.maxLife = particle.life;
      particle.color = color;
      break;
    }
  }
}

void splitBalls() {
  Ball* source = nullptr;
  for (auto& currentBall : balls) {
    if (currentBall.active && !currentBall.stuck) {
      source = &currentBall;
      break;
    }
  }
  if (source == nullptr) {
    source = firstActiveBall();
  }
  if (source == nullptr) {
    return;
  }

  size_t currentCount = activeBallCount();
  if (currentCount >= balls.size()) {
    return;
  }

  static constexpr float kOffsets[] = {-0.42f, 0.42f};
  size_t offsetIndex = 0;
  for (auto& extraBall : balls) {
    if (extraBall.active) {
      continue;
    }

    extraBall = *source;
    extraBall.active = true;
    extraBall.stuck = false;
    const float baseVx = source->vx;
    const float offset = kOffsets[offsetIndex % 2];
    extraBall.vx = baseVx + currentBallSpeed() * offset;
    extraBall.vy = source->vy * (0.94f - 0.04f * offsetIndex);
    normalizeBallSpeed(extraBall);

    ++currentCount;
    ++offsetIndex;
    if (currentCount >= balls.size()) {
      break;
    }
  }

  source->stuck = false;
  normalizeBallSpeed(*source);
}

void launchBall() {
  Ball* ball = firstStuckBall();
  if (ball == nullptr) {
    return;
  }

  ball->stuck = false;
  const float speed = currentBallSpeed();
  const float angle = lerpf(-0.95f, 0.95f, static_cast<float>(random(0, 1000)) / 999.0f);
  ball->vx = speed * angle * 0.55f;
  ball->vy = -speed;
  normalizeBallSpeed(*ball);
  queueTone(2800, 18, 4);
  queueTone(3600, 22, 10);
}

void fireLaser() {
  if (millis() >= laserUntilMs || millis() < laserCooldownMs) {
    return;
  }

  for (size_t i = 0; i + 1 < lasers.size(); i += 2) {
    if (!lasers[i].active && !lasers[i + 1].active) {
      lasers[i].active = true;
      lasers[i].x = paddle.x + 8.0f;
      lasers[i].y = kPaddleY - 2.0f;
      lasers[i].vy = -230.0f;

      lasers[i + 1].active = true;
      lasers[i + 1].x = paddle.x + paddle.w - 8.0f;
      lasers[i + 1].y = kPaddleY - 2.0f;
      lasers[i + 1].vy = -230.0f;
      laserCooldownMs = millis() + 165;
      sfxLaser();
      return;
    }
  }
}

void openOptions(const GameState fromState) {
  returnState = fromState;
  gameState = GameState::Options;
  optionsIndex = 0;
  waitingForBinding = false;
  uiInputUnlockMs = millis() + 120;
}

void closeOptions() {
  saveConfig();
  applyAudioConfig();
  if (!config.randomBackground) {
    activeBackgroundIndex = clampBackgroundIndex(config.backgroundIndex);
    invalidateBackgroundCache();
  }
  gameState = returnState;
  if (gameState == GameState::Title) {
    syncAudioPlayback();
  }
}

void openHelp(const GameState fromState) {
  returnState = fromState;
  gameState = GameState::Help;
  uiInputUnlockMs = millis() + 120;
}

void scanMusicFiles() {
  availableTracks.clear();
  if (!sdReady || !lockSD()) {
    return;
  }

  if (!SD.exists("/ADVnoid/music")) {
    SD.mkdir("/ADVnoid/music");
  }

  File dir = SD.open("/ADVnoid/music");
  if (dir) {
    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        String lower = name;
        name.trim();
        lower.trim();
        lower.toLowerCase();
        if (lower.endsWith(".mp3")) {
          const int slash = name.lastIndexOf('/');
          availableTracks.push_back(slash >= 0 ? name.substring(slash + 1) : name);
        }
      }
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();
  }

  unlockSD();
  std::sort(availableTracks.begin(), availableTracks.end(),
            [](const String& a, const String& b) { return a.compareTo(b) < 0; });
}

void syncAudioPlayback(const bool forceRestart) {
  AudioTask::mp3Enabled = config.mp3Enabled;
  AudioTask::setMP3Volume(mp3VolumeValue());
  if (!config.mp3Enabled) {
    AudioTask::stop();
    return;
  }

  AudioTask::loadPlaylist();
  if (AudioTask::getPlaylistCopy().empty()) {
    AudioTask::stop();
    return;
  }

  if (forceRestart || !AudioTask::isPlaying()) {
    AudioTask::startPlaylist();
  }
}

void openPlaylistEditor() {
  returnState = GameState::Options;
  scanMusicFiles();
  AudioTask::loadPlaylist();
  playlistDraft = AudioTask::getPlaylistCopy();
  playlistCursor = 0;
  gameState = GameState::Playlist;
  uiInputUnlockMs = millis() + 120;
}

bool pointInBrick(const float x, const float y, const Brick& brick) {
  return brick.alive && x >= brick.x && x <= brick.x + brick.w &&
         y >= brick.y && y <= brick.y + brick.h;
}

void hitBrick(Brick& brick) {
  if (!brick.alive) {
    return;
  }

  if (!brick.breakable) {
    queueTone(brick.motionRange > 0.0f ? 1080 : 880, 10, 4);
    return;
  }

  sfxBrick();
  const uint16_t impactColor = brick.color;
  spawnBrickParticles(brick, impactColor, brick.hits > 1 ? 6 : 12);
  score += brick.hits > 1 ? 35 : 60;
  if (brick.hits > 1) {
    --brick.hits;
    brick.color = brickColorForHits(brick.hits, brick.colorVariant);
    return;
  }

  brick.alive = false;
  if (breakableLeft > 0) {
    --breakableLeft;
  }
  spawnPowerUp(brick.x + brick.w * 0.5f, brick.y + brick.h * 0.5f);
}

void loseLife() {
  sfxLoseLife();
  if (lives > 0) {
    --lives;
  }

  if (lives == 0) {
    if (!scoreStoredThisRound) {
      beginNameEntry(score);
    } else {
      gameState = GameState::GameOver;
      stateTimerMs = millis();
    }
    sfxGameOver();
    return;
  }

  clearEntities();
  resetBallsOnPaddle();
}

void updatePowerUps(const float dt) {
  for (auto& power : powerUps) {
    if (!power.active) {
      continue;
    }

    power.y += power.vy * dt;
    const bool hitPaddle =
        power.y >= kPaddleY - 6.0f && power.y <= kPaddleY + 8.0f &&
        power.x >= paddle.x - 6.0f && power.x <= paddle.x + paddle.w + 6.0f;
    if (hitPaddle) {
      power.active = false;
      score += 120;
      switch (power.type) {
        case PowerType::Expand:
          expandUntilMs = millis() + 15000;
          paddle.w = currentPaddleWidth();
          break;
        case PowerType::Laser:
          laserUntilMs = millis() + 15000;
          break;
        case PowerType::Slow:
          slowUntilMs = millis() + 12000;
          for (auto& ball : balls) {
            if (ball.active) {
              normalizeBallSpeed(ball);
            }
          }
          break;
        case PowerType::MultiBall:
          splitBalls();
          break;
      }
      sfxPower();
    } else if (power.y > kScreenH + 8.0f) {
      power.active = false;
    }
  }
}

void updateParticles(const float dt) {
  for (auto& particle : particles) {
    if (!particle.active) {
      continue;
    }

    particle.x += particle.vx * dt;
    particle.y += particle.vy * dt;
    particle.vy += 160.0f * dt;
    particle.vx *= 0.98f;
    particle.life -= dt;
    if (particle.life <= 0.0f || particle.y > kScreenH + 4.0f) {
      particle.active = false;
    }
  }
}

void updateLasers(const float dt) {
  for (auto& shot : lasers) {
    if (!shot.active) {
      continue;
    }

    shot.y += shot.vy * dt;
    if (shot.y < kArenaTop) {
      shot.active = false;
      continue;
    }

    for (auto& brick : bricks) {
      if (!pointInBrick(shot.x, shot.y, brick)) {
        continue;
      }
      hitBrick(brick);
      shot.active = false;
      break;
    }
  }
}

void updateWalls() {
  const float now = static_cast<float>(millis()) * 0.001f;
  for (auto& brick : bricks) {
    if (!brick.alive || brick.motionRange <= 0.0f) {
      continue;
    }
    brick.x = brick.baseX + std::sin(now * brick.motionSpeed + brick.motionPhase) *
                                brick.motionRange;
  }
}

void updateBall(Ball& ball, const float dt, const bool emitTrail) {
  if (!ball.active) {
    return;
  }

  if (ball.stuck) {
    ball.x = paddle.x + paddle.w * 0.5f;
    ball.y = kPaddleY - ball.r - 1.0f;
    return;
  }

  const float prevX = ball.x;
  const float prevY = ball.y;
  ball.x += ball.vx * dt;
  ball.y += ball.vy * dt;
  if (config.trails && emitTrail) {
    pushTrail(ball.x, ball.y);
  }

  if (ball.x - ball.r <= kArenaLeft) {
    ball.x = kArenaLeft + ball.r;
    ball.vx = std::fabs(ball.vx);
    queueTone(900, 8, 4);
  } else if (ball.x + ball.r >= kArenaRight) {
    ball.x = kArenaRight - ball.r;
    ball.vx = -std::fabs(ball.vx);
    queueTone(900, 8, 4);
  }

  if (ball.y - ball.r <= kArenaTop) {
    ball.y = kArenaTop + ball.r;
    ball.vy = std::fabs(ball.vy);
    queueTone(1200, 10, 4);
  }

  const bool paddleHit =
      ball.vy > 0.0f && ball.y + ball.r >= kPaddleY &&
      ball.y - ball.r <= kPaddleY + 8.0f &&
      ball.x >= paddle.x - ball.r && ball.x <= paddle.x + paddle.w + ball.r;
  if (paddleHit) {
    ball.y = kPaddleY - ball.r;
    const float hit = ((ball.x - paddle.x) / paddle.w) - 0.5f;
    ball.vx = currentBallSpeed() * hit * 1.55f;
    ball.vy = -std::fabs(currentBallSpeed() * (0.84f - std::fabs(hit) * 0.18f));
    normalizeBallSpeed(ball);
    sfxPaddle();
  }

  for (auto& brick : bricks) {
    if (!brick.alive) {
      continue;
    }

    const float closestX = clampf(ball.x, brick.x, brick.x + brick.w);
    const float closestY = clampf(ball.y, brick.y, brick.y + brick.h);
    const float dx = ball.x - closestX;
    const float dy = ball.y - closestY;
    if (dx * dx + dy * dy > ball.r * ball.r) {
      continue;
    }

    hitBrick(brick);

    const float prevLeftEdge = prevX - ball.r;
    const float prevRightEdge = prevX + ball.r;
    const float prevTopEdge = prevY - ball.r;
    const float prevBottomEdge = prevY + ball.r;

    if (prevBottomEdge <= brick.y) {
      ball.vy = -std::fabs(ball.vy);
      ball.y = brick.y - ball.r;
    } else if (prevTopEdge >= brick.y + brick.h) {
      ball.vy = std::fabs(ball.vy);
      ball.y = brick.y + brick.h + ball.r;
    } else if (prevRightEdge <= brick.x) {
      ball.vx = -std::fabs(ball.vx);
      ball.x = brick.x - ball.r;
    } else if (prevLeftEdge >= brick.x + brick.w) {
      ball.vx = std::fabs(ball.vx);
      ball.x = brick.x + brick.w + ball.r;
    } else {
      ball.vy = -ball.vy;
    }

    normalizeBallSpeed(ball);
    break;
  }

  if (ball.y - ball.r > kScreenH + 2.0f) {
    ball.active = false;
    ball.stuck = false;
  }
}

void updateTitle(const InputState& input) {
  titleAnimMs = millis();
  if (millis() - stateTimerMs >= (kTitleScreenMs + kScoreScreenMs)) {
    stateTimerMs = millis();
  }
  if (handleKonamiCodeOnTitle(input)) {
    return;
  }
  if (input.actionPressed) {
    newGame();
  } else if (input.optionsPressed) {
    resetKonamiProgress();
    openOptions(GameState::Title);
  } else if (input.helpPressed) {
    resetKonamiProgress();
    openHelp(GameState::Title);
  }
}

void updateKonamiReveal() {
  const uint32_t elapsed = millis() - stateTimerMs;
  if (elapsed >= (kKonamiFirstRevealMs + kKonamiSecondRevealMs)) {
    setGyroControlsEnabled(true);
    gameState = GameState::Title;
    stateTimerMs = millis();
  }
}

void updateGyroOff() {
  if (millis() - stateTimerMs >= kGyroOffScreenMs) {
    gameState = GameState::Title;
    stateTimerMs = millis();
  }
}

void updateOptions(const InputState& input) {
  if (input.optionsPressed) {
    if (waitingForBinding) {
      waitingForBinding = false;
    } else {
      closeOptions();
    }
    return;
  }

  if (waitingForBinding) {
    const char typed = input.typedChar;
    if (typed == 0) {
      return;
    }

    char* binding = optionBindingRef(optionsIndex);
    if (binding == nullptr || isReservedBinding(typed) || isDuplicateBinding(typed, optionsIndex)) {
      queueTone(740, 60, 8);
      return;
    }

    *binding = typed;
    waitingForBinding = false;
    queueTone(2600, 24, 4);
    queueTone(3200, 30, 8);
    return;
  }

  if (input.menuPrevPressed) {
    optionsIndex = (optionsIndex + kOptionCount - 1) % kOptionCount;
    queueTone(1800, 14, 4);
    return;
  }

  if (input.menuNextPressed) {
    optionsIndex = (optionsIndex + 1) % kOptionCount;
    queueTone(2040, 14, 4);
    return;
  }

  if (input.actionPressed) {
    switch (optionsIndex) {
      case 0:
        config.sfx = !config.sfx;
        break;
      case 1:
        config.volumeStep = static_cast<uint8_t>((config.volumeStep + 1) % 11);
        break;
      case 2:
        config.difficulty = static_cast<uint8_t>((config.difficulty + 1) % 3);
        break;
      case 3:
        config.trails = !config.trails;
        break;
      case 4:
        config.paddleSize = static_cast<uint8_t>((config.paddleSize + 1) % 3);
        paddle.w = currentPaddleWidth();
        for (auto& ball : balls) {
          if (ball.active && ball.stuck) {
            ball.x = paddle.x + paddle.w * 0.5f;
          }
        }
        break;
      case 5:
        config.randomBackground = !config.randomBackground;
        if (!config.randomBackground) {
          activeBackgroundIndex = clampBackgroundIndex(config.backgroundIndex);
          invalidateBackgroundCache();
        }
        break;
      case 6:
      case 7:
      case 8:
        waitingForBinding = true;
        queueTone(1800, 18, 4);
        queueTone(2200, 18, 8);
        return;
      case 9:
        config.mp3Enabled = !config.mp3Enabled;
        syncAudioPlayback();
        break;
      case 10:
        config.mp3VolumeStep = static_cast<uint8_t>((config.mp3VolumeStep + 1) % 11);
        AudioTask::setMP3Volume(mp3VolumeValue());
        break;
      case 11:
        AudioTask::nextTrack();
        break;
      case 12:
        openPlaylistEditor();
        return;
      case 13:
        saveConfig();
        applyAudioConfig();
        returnToTitle();
        return;
      default:
        break;
    }
    applyAudioConfig();
    queueTone(2200 + optionsIndex * 240, 16, 4);
    return;
  }
}

void updatePlaying(const float dt, const InputState& input) {
  if (input.pausePressed) {
    gameState = GameState::Pause;
    queueTone(1350, 18, 4);
    queueTone(980, 26, 8);
    return;
  }
  if (input.helpPressed) {
    if (config.randomBackground) {
      queueTone(740, 60, 8);
    } else {
      openBackgroundSelector(GameState::Playing);
    }
    return;
  }
  if (input.optionsPressed) {
    returnToTitle();
    return;
  }

  paddle.w = currentPaddleWidth();
  const float keyboardDirection = (input.right ? 1.0f : 0.0f) - (input.left ? 1.0f : 0.0f);
  const float direction = clampf(keyboardDirection + input.motionTurn, -1.0f, 1.0f);
  paddle.x += direction * currentPaddleSpeed() * dt;
  paddle.x = clampf(paddle.x, kArenaLeft + 2.0f, kArenaRight - paddle.w - 2.0f);

  updateWalls();

  const int trailIndex = trailBallIndex();
  for (size_t i = 0; i < balls.size(); ++i) {
    updateBall(balls[i], dt, static_cast<int>(i) == trailIndex);
  }

  const bool firePressed = input.actionPressed || input.motionActionPressed;
  if (firePressed && firstStuckBall() != nullptr) {
    launchBall();
  }

  if (millis() < laserUntilMs && firePressed && hasLaunchedBall()) {
    fireLaser();
  }

  updateLasers(dt);
  updatePowerUps(dt);

  if (activeBallCount() == 0) {
    loseLife();
    return;
  }

  updateParticles(dt);
  if (breakableLeft == 0) {
    gameState = GameState::LevelClear;
    stateTimerMs = millis();
    sfxLevelClear();
  }
}

void updateBackgroundSelect(const InputState& input) {
  if (input.menuPrevPressed) {
    config.backgroundIndex = clampBackgroundIndex(
        (static_cast<int>(config.backgroundIndex) - 1 + static_cast<int>(kBackgroundAssets.size())) %
        static_cast<int>(kBackgroundAssets.size()));
    invalidateBackgroundCache();
    queueTone(1420, 14, 4);
  } else if (input.menuNextPressed) {
    config.backgroundIndex = clampBackgroundIndex(
        (static_cast<int>(config.backgroundIndex) + 1) % static_cast<int>(kBackgroundAssets.size()));
    invalidateBackgroundCache();
    queueTone(1680, 14, 4);
  } else if (input.helpPressed || input.actionPressed ||
             input.optionsPressed) {
    activeBackgroundIndex = clampBackgroundIndex(config.backgroundIndex);
    invalidateBackgroundCache();
    saveConfig();
    if (returnState == GameState::Options) {
      uiInputUnlockMs = millis() + 120;
    }
    gameState = returnState;
  }
}

void updatePause(const InputState& input) {
  if (input.pausePressed || input.actionPressed) {
    gameState = GameState::Playing;
    queueTone(980, 18, 4);
    queueTone(1450, 24, 8);
  } else if (input.optionsPressed) {
    returnToTitle();
  } else if (input.helpPressed) {
    openHelp(GameState::Pause);
  }
}

void updateLevelClear() {
  if (millis() - stateTimerMs < 1200) {
    return;
  }

  ++level;
  generateLevel();
  gameState = GameState::Playing;
}

void updateGameOver(const InputState& input) {
  if (input.actionPressed) {
    returnToTitle();
  } else if (input.optionsPressed) {
    openOptions(GameState::Title);
  } else if (input.helpPressed) {
    openHelp(GameState::GameOver);
  }
}

void updatePlaylist(const InputState& input) {
  if (input.optionsPressed) {
    AudioTask::savePlaylist(playlistDraft);
    if (config.mp3Enabled) {
      syncAudioPlayback(true);
    }
    uiInputUnlockMs = millis() + 120;
    gameState = GameState::Options;
    return;
  }

  if (availableTracks.empty()) {
    return;
  }

  if (input.menuPrevPressed) {
    playlistCursor--;
    if (playlistCursor < 0) {
      playlistCursor = static_cast<int>(availableTracks.size()) - 1;
    }
    queueTone(1800, 14, 4);
  } else if (input.menuNextPressed) {
    playlistCursor++;
    if (playlistCursor >= static_cast<int>(availableTracks.size())) {
      playlistCursor = 0;
    }
    queueTone(2040, 14, 4);
  } else if (input.actionPressed) {
    togglePlaylistTrack(availableTracks[static_cast<size_t>(playlistCursor)]);
    queueTone(2400, 18, 6);
  }
}

void updateNameEntry(const InputState& input) {
  if (input.leftPressed) {
    char& current = nameEntryChars[nameEntryIndex];
    current = stepNameChar(current, -1);
    queueTone(1700, 14, 4);
  } else if (input.rightPressed) {
    char& current = nameEntryChars[nameEntryIndex];
    current = stepNameChar(current, 1);
    queueTone(2100, 14, 4);
  } else if (input.actionPressed) {
    if (nameEntryIndex < 2) {
      ++nameEntryIndex;
      queueTone(2500, 18, 4);
    } else {
      finalizeNameEntry();
      queueTone(2800, 24, 8);
      queueTone(3400, 28, 10);
    }
  }
}

void updateHelp(const InputState& input) {
  if (input.helpPressed || input.actionPressed) {
    gameState = returnState;
  } else if (input.optionsPressed) {
    openOptions(returnState);
  } else if (input.pausePressed && returnState == GameState::Pause) {
    gameState = GameState::Playing;
  }
}

InputState readInput() {
  M5Cardputer.update();
  auto& keyboard = M5Cardputer.Keyboard;
  const Keyboard_Class::KeysState state = keyboard.keysState();
  const bool keyChanged = keyboard.isChange() && keyboard.isPressed();

  const char leftKey = normalizeBindingChar(config.leftKey);
  const char rightKey = normalizeBindingChar(config.rightKey);
  const char actionKey = normalizeBindingChar(config.actionKey);

  const bool left = keyboard.isKeyPressed(leftKey) || normalizedWordHas(state, leftKey);
  const bool right = keyboard.isKeyPressed(rightKey) || normalizedWordHas(state, rightKey);
  const bool action =
      keyboard.isKeyPressed(actionKey) || normalizedWordHas(state, actionKey);
  const bool pause = keyboard.isKeyPressed('b') || normalizedWordHas(state, 'b');
  const bool trackPrev = keyboard.isKeyPressed(',') || wordHas(state, ',');
  const bool trackNext = keyboard.isKeyPressed('/') || wordHas(state, '/');
  const bool up = keyboard.isKeyPressed('e') || normalizedWordHas(state, 'e');
  const bool down = keyboard.isKeyPressed('z') || normalizedWordHas(state, 'z');
  const bool konamiB = keyboard.isKeyPressed('k') || normalizedWordHas(state, 'k');
  const bool options = M5Cardputer.BtnA.isPressed();
  const bool help = keyboard.isKeyPressed('v') || normalizedWordHas(state, 'v') ||
                    hidHas(state, HID_V);
  const bool menuPrev = keyboard.isKeyPressed('a') || normalizedWordHas(state, 'a');
  const bool menuNext = keyboard.isKeyPressed('d') || normalizedWordHas(state, 'd');

  InputState input{};
  input.left = left;
  input.right = right;
  input.action = action;
  input.pause = pause;
  input.trackPrev = trackPrev;
  input.trackNext = trackNext;
  input.options = options;
  input.help = help;
  input.menuPrev = menuPrev;
  input.menuNext = menuNext;
  input.leftPressed = left && !prevLeft;
  input.rightPressed = right && !prevRight;
  input.actionPressed = action && !prevAction;
  input.pausePressed = pause && !prevPause;
  input.trackPrevPressed = trackPrev && !prevTrackPrev;
  input.trackNextPressed = trackNext && !prevTrackNext;
  input.optionsPressed = M5Cardputer.BtnA.wasPressed();
  input.helpPressed = help && !prevHelp;
  input.menuPrevPressed = menuPrev && !prevMenuPrev;
  input.menuNextPressed = menuNext && !prevMenuNext;
  input.upPressed = up && keyChanged;
  input.downPressed = down && keyChanged;
  input.konamiBPressed = konamiB && keyChanged;
  input.typedChar = keyChanged ? firstPrintableNormalized(state) : 0;
  updateMotionInput(input);

  prevLeft = left;
  prevRight = right;
  prevAction = action;
  prevPause = pause;
  prevTrackPrev = trackPrev;
  prevTrackNext = trackNext;
  prevOptions = options;
  prevHelp = help;
  prevMenuPrev = menuPrev;
  prevMenuNext = menuNext;
  return input;
}

void drawGlassPanel(const int x, const int y, const int w, const int h,
                    const uint16_t baseColor, const uint16_t glowColor) {
  canvas.fillRoundRect(x, y, w, h, 7, mix565(baseColor, BLACK, 0.45f));
  canvas.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 6, baseColor);
  canvas.fillRoundRect(x + 2, y + 2, w - 4, std::max(4, h / 2 - 1), 5,
                       mix565(baseColor, WHITE, 0.48f));
  canvas.drawRoundRect(x, y, w, h, 7, mix565(glowColor, WHITE, 0.50f));
  canvas.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6,
                       mix565(baseColor, WHITE, 0.35f));
  canvas.drawFastHLine(x + 6, y + 4, w - 12, mix565(glowColor, WHITE, 0.75f));
}

void drawBackdrop() {
  const uint16_t top = rgb565(255, 188, 224);
  const uint16_t mid = rgb565(164, 216, 255);
  const uint16_t bottom = rgb565(158, 244, 220);

  for (int y = 0; y < kScreenH; ++y) {
    const float t = static_cast<float>(y) / static_cast<float>(kScreenH - 1);
    const uint16_t row = t < 0.52f ? mix565(top, mid, t / 0.52f)
                                   : mix565(mid, bottom, (t - 0.52f) / 0.48f);
    canvas.drawFastHLine(0, y, kScreenW, row);
  }

  for (int y = 12; y < kScreenH; y += 22) {
    canvas.drawFastHLine(0, y, kScreenW, mix565(mid, WHITE, 0.32f));
    canvas.drawFastHLine(0, y + 1, kScreenW, mix565(mid, rgb565(136, 176, 242), 0.22f));
  }

  for (int x = -20; x < kScreenW + 24; x += 32) {
    for (int y = -8; y < kScreenH + 8; y += 24) {
      const uint16_t diamond = ((x + y) / 8) % 2 == 0 ? rgb565(255, 232, 170)
                                                       : rgb565(224, 194, 255);
      canvas.drawLine(x, y + 8, x + 8, y, mix565(diamond, WHITE, 0.34f));
      canvas.drawLine(x + 8, y, x + 16, y + 8, mix565(diamond, WHITE, 0.34f));
      canvas.drawLine(x + 16, y + 8, x + 8, y + 16, mix565(diamond, rgb565(156, 186, 248), 0.28f));
      canvas.drawLine(x + 8, y + 16, x, y + 8, mix565(diamond, rgb565(156, 186, 248), 0.28f));
    }
  }

  for (int x = 14; x < kScreenW; x += 36) {
    for (int y = 10; y < kScreenH; y += 28) {
      const uint16_t accent = ((x / 36) + (y / 28)) % 2 == 0 ? rgb565(255, 176, 208)
                                                              : rgb565(156, 232, 255);
      canvas.fillCircle(x, y, 2, mix565(accent, WHITE, 0.24f));
      canvas.drawCircle(x, y, 4, mix565(accent, rgb565(156, 178, 238), 0.22f));
    }
  }
}

void drawMoireBackdrop() {
  const float t = static_cast<float>(millis()) * 0.0012f;
  const float cx1 = sinf(t * 0.53f) * 80.0f + 120.0f;
  const float cy1 = cosf(t * 0.31f) * 42.0f + 67.0f;
  const float cx2 = cosf(t * 0.41f) * 84.0f + 120.0f;
  const float cy2 = sinf(t * 0.87f) * 38.0f + 67.0f;

  const uint16_t dark = rgb565(8, 14, 26);
  const uint16_t mid = rgb565(24, 54, 102);
  const uint16_t glow = rgb565(152, 228, 255);

  for (int y = 0; y < kScreenH; ++y) {
    const float dy1sq = (static_cast<float>(y) - cy1) * (static_cast<float>(y) - cy1);
    const float dy2sq = (static_cast<float>(y) - cy2) * (static_cast<float>(y) - cy2);
    for (int x = 0; x < kScreenW; ++x) {
      const float dx1 = static_cast<float>(x) - cx1;
      const float dx2 = static_cast<float>(x) - cx2;
      const int dist1 = static_cast<int>(std::sqrt(dx1 * dx1 + dy1sq));
      const int dist2 = static_cast<int>(std::sqrt(dx2 * dx2 + dy2sq));
      const int pattern = ((dist1 ^ dist2) + ((x + y) >> 2)) & 0x1F;

      uint16_t color = dark;
      if (pattern < 6) {
        color = glow;
      } else if (pattern < 12) {
        color = mix565(glow, mid, 0.58f);
      } else if (pattern < 18) {
        color = mid;
      } else if (pattern < 24) {
        color = mix565(mid, dark, 0.45f);
      }
      canvas.drawPixel(x, y, color);
    }
  }
}

void drawFireBackdrop() {
  canvas.fillScreen(BLACK);

  for (int y = 1; y < kScreenH; ++y) {
    const int row = y * kScreenW;
    const int nextRow = (y + 1) * kScreenW;
    for (int x = 1; x < kScreenW - 1; ++x) {
      int value = (fireBuffer[static_cast<size_t>(nextRow + x)] +
                   fireBuffer[static_cast<size_t>(nextRow + x - 1)] +
                   fireBuffer[static_cast<size_t>(nextRow + x + 1)] +
                   fireBuffer[static_cast<size_t>(nextRow + kScreenW + x)]) >>
                  2;
      value = value > 2 ? value - 2 : 0;
      fireBuffer[static_cast<size_t>(row + x)] = static_cast<uint8_t>(value);
      canvas.drawPixel(x, y, firePalette[static_cast<size_t>(value)]);
    }
  }

  const int lastRow = (kScreenH - 1) * kScreenW;
  for (int x = 0; x < kScreenW; ++x) {
    fireBuffer[static_cast<size_t>(lastRow + x)] =
        static_cast<uint8_t>(random(100) > 46 ? 255 : 0);
  }
}

void drawLevelBackground() {
  refreshBackgroundCache();
  if (backgroundCanvasPtr != nullptr) {
    backgroundCanvas.pushSprite(&canvas, 0, 0);
  } else {
    canvas.fillScreen(rgb565(8, 12, 26));
  }
}

void drawArenaField() {
  const int x = 3;
  const int y = 15;
  const int w = 234;
  const int h = 116;
  const uint16_t border = rgb565(154, 204, 255);
  const uint16_t shell = rgb565(86, 122, 186);
  const uint16_t inner = rgb565(16, 36, 74);

  canvas.fillRect(x, y, w, h, rgb565(18, 22, 34));
  canvas.drawRect(x + 1, y + 1, w - 2, h - 2, shell);
  canvas.drawRect(x + 2, y + 2, w - 4, h - 4, mix565(shell, WHITE, 0.28f));
  canvas.drawRect(x + 3, y + 3, w - 6, h - 6, inner);

  for (int gy = x + 12; gy < x + w - 8; gy += 22) {
    canvas.drawFastVLine(gy, y + 8, h - 16, mix565(shell, WHITE, 0.10f));
  }
  for (int gx = y + 12; gx < y + h - 8; gx += 18) {
    canvas.drawFastHLine(x + 8, gx, w - 16, mix565(inner, WHITE, 0.10f));
  }

  for (int i = 0; i < 6; ++i) {
    const int px = x + 26 + i * 32;
    const int py = y + 14 + (i % 3) * 24;
    const uint16_t shard = i % 2 == 0 ? rgb565(128, 214, 255) : rgb565(228, 168, 255);
    canvas.drawLine(px - 6, py, px, py - 6, mix565(shard, WHITE, 0.32f));
    canvas.drawLine(px, py - 6, px + 6, py, mix565(shard, WHITE, 0.32f));
    canvas.drawLine(px + 6, py, px, py + 6, mix565(shard, WHITE, 0.32f));
    canvas.drawLine(px, py + 6, px - 6, py, mix565(shard, WHITE, 0.32f));
  }
}

void drawSpritePanel(const int x, const int y, const int w, const int h,
                     const uint16_t shell, const uint16_t fill,
                     const uint16_t hi = 0, const uint16_t low = 0) {
  const uint16_t outline = rgb565(18, 22, 34);
  const uint16_t highlight = hi == 0 ? mix565(fill, WHITE, 0.34f) : hi;
  const uint16_t shadow = low == 0 ? mix565(fill, BLACK, 0.26f) : low;

  canvas.fillRect(x, y, w, h, outline);
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, shell);
  canvas.fillRect(x + 2, y + 2, w - 4, h - 4, fill);
  canvas.drawFastHLine(x + 2, y + 2, w - 4, highlight);
  canvas.drawFastHLine(x + 2, y + h - 3, w - 4, shadow);
  canvas.drawFastVLine(x + 2, y + 2, h - 4, mix565(highlight, fill, 0.22f));
  canvas.drawFastVLine(x + w - 3, y + 2, h - 4, mix565(shadow, outline, 0.18f));
}

void drawArcadeBrickSprite(const Brick& brick) {
  const int x = static_cast<int>(brick.x);
  const int y = static_cast<int>(brick.y);
  const int w = static_cast<int>(brick.w);
  const int h = static_cast<int>(brick.h);
  const uint16_t outline = rgb565(18, 24, 38);
  const uint16_t frame = mix565(brick.color, rgb565(28, 32, 48), 0.35f);
  const uint16_t fill = mix565(brick.color, WHITE, 0.08f);
  const uint16_t hi = mix565(brick.color, WHITE, 0.42f);
  const uint16_t low = mix565(brick.color, BLACK, 0.38f);

  canvas.fillRect(x, y, w, h, outline);
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, frame);
  canvas.fillRect(x + 2, y + 2, w - 4, h - 4, fill);
  canvas.drawFastHLine(x + 2, y + 2, w - 4, hi);
  canvas.drawFastHLine(x + 2, y + 3, w - 4, mix565(hi, fill, 0.45f));
  canvas.drawFastHLine(x + 2, y + h - 3, w - 4, low);
  canvas.drawFastVLine(x + 2, y + 2, h - 4, mix565(hi, fill, 0.30f));
  canvas.drawFastVLine(x + w - 3, y + 2, h - 4, mix565(low, outline, 0.25f));

  if (w >= 16) {
    const int seamX = x + w / 2;
    canvas.drawFastVLine(seamX, y + 3, h - 6, mix565(fill, BLACK, 0.10f));
    canvas.drawFastVLine(seamX - 1, y + 3, h - 6, mix565(fill, WHITE, 0.12f));
  }

  if (brick.hits > 1) {
    const uint16_t pip = brick.hits >= 3 ? rgb565(238, 228, 255) : rgb565(255, 232, 244);
    if (brick.hits >= 3) {
      canvas.fillRect(x + w / 2 - 4, y + h / 2 - 2, 3, 3, pip);
      canvas.fillRect(x + w / 2 + 1, y + h / 2 - 2, 3, 3, pip);
      canvas.drawRect(x + w / 2 - 4, y + h / 2 - 2, 3, 3, rgb565(72, 54, 110));
      canvas.drawRect(x + w / 2 + 1, y + h / 2 - 2, 3, 3, rgb565(72, 54, 110));
    } else {
      canvas.fillRect(x + w / 2 - 2, y + h / 2 - 2, 4, 3, pip);
      canvas.drawRect(x + w / 2 - 2, y + h / 2 - 2, 4, 3, rgb565(136, 144, 164));
    }
  }
}

void drawWallSprite(const Brick& brick) {
  const int x = static_cast<int>(brick.x);
  const int y = static_cast<int>(brick.y);
  const int w = static_cast<int>(brick.w);
  const int h = static_cast<int>(brick.h);
  const bool moving = brick.motionRange > 0.0f;

  const uint16_t outline = rgb565(20, 24, 34);
  const uint16_t shell = moving ? rgb565(62, 100, 122) : rgb565(118, 126, 142);
  const uint16_t core = moving ? rgb565(116, 206, 222) : rgb565(198, 206, 220);
  const uint16_t shade = moving ? rgb565(22, 68, 90) : rgb565(136, 144, 158);

  canvas.fillRect(x, y, w, h, outline);
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, shell);
  canvas.fillRect(x + 2, y + 2, w - 4, h - 4, core);
  canvas.drawFastHLine(x + 2, y + 2, w - 4, mix565(core, WHITE, 0.45f));
  canvas.drawFastHLine(x + 2, y + h - 3, w - 4, mix565(shade, BLACK, 0.18f));
  canvas.drawFastVLine(x + 2, y + 2, h - 4, mix565(core, WHITE, 0.25f));
  canvas.drawFastVLine(x + w - 3, y + 2, h - 4, mix565(shade, BLACK, 0.22f));
  canvas.drawFastVLine(x + w / 2, y + 3, h - 6, mix565(shell, BLACK, 0.12f));

  if (moving) {
    canvas.drawFastHLine(x + 4, y + h / 2, w - 8, rgb565(220, 255, 255));
    canvas.fillRect(x + 4, y + 3, 2, h - 6, rgb565(210, 255, 255));
    canvas.fillRect(x + w - 6, y + 3, 2, h - 6, rgb565(210, 255, 255));
  } else {
    canvas.fillCircle(x + 3, y + 3, 1, rgb565(236, 240, 248));
    canvas.fillCircle(x + w - 4, y + 3, 1, rgb565(236, 240, 248));
    canvas.fillCircle(x + 3, y + h - 4, 1, rgb565(236, 240, 248));
    canvas.fillCircle(x + w - 4, y + h - 4, 1, rgb565(236, 240, 248));
  }
}

void drawBricks() {
  for (const auto& brick : bricks) {
    if (!brick.alive) {
      continue;
    }

    if (!brick.breakable) {
      drawWallSprite(brick);
    } else {
      drawArcadeBrickSprite(brick);
    }
  }
}

void drawPaddle() {
  const int x = static_cast<int>(paddle.x);
  const int y = kPaddleY;
  const int w = static_cast<int>(paddle.w);
  const int h = 7;
  const uint16_t outline = rgb565(18, 18, 28);
  const uint16_t wing = rgb565(166, 172, 188);
  const uint16_t wingShade = rgb565(82, 88, 104);
  const uint16_t bodyDark = rgb565(132, 28, 38);
  const uint16_t bodyMid = rgb565(212, 58, 62);
  const uint16_t bodyHi = rgb565(255, 146, 112);

  canvas.fillRoundRect(x, y, w, h, 3, outline);
  canvas.fillRect(x + 1, y + 2, 3, h - 4, wing);
  canvas.fillRect(x + w - 4, y + 2, 3, h - 4, wing);
  canvas.fillRect(x + 2, y + 2, 1, h - 4, mix565(wing, WHITE, 0.35f));
  canvas.fillRect(x + w - 3, y + 2, 1, h - 4, mix565(wingShade, BLACK, 0.20f));

  canvas.fillRoundRect(x + 3, y + 1, w - 6, h - 2, 2, bodyDark);
  canvas.fillRoundRect(x + 4, y + 2, w - 8, h - 4, 2, bodyMid);
  canvas.drawFastHLine(x + 5, y + 2, w - 10, bodyHi);
  canvas.drawFastHLine(x + 5, y + h - 3, w - 10, mix565(bodyDark, BLACK, 0.18f));

  const int coreX = x + w / 2 - 5;
  canvas.fillRoundRect(coreX, y + 1, 10, h - 2, 2, rgb565(196, 202, 214));
  canvas.fillRoundRect(coreX + 1, y + 2, 8, h - 4, 1, rgb565(112, 188, 150));
  canvas.drawFastHLine(coreX + 2, y + 2, 6, rgb565(208, 255, 210));

  if (millis() < laserUntilMs) {
    canvas.fillRect(x + 3, y - 2, 2, 3, rgb565(255, 240, 180));
    canvas.fillRect(x + w - 5, y - 2, 2, 3, rgb565(255, 240, 180));
    canvas.drawFastVLine(x + 4, y - 4, 3, rgb565(255, 176, 92));
    canvas.drawFastVLine(x + w - 4, y - 4, 3, rgb565(255, 176, 92));
  }
}

void drawBalls() {
  if (config.trails) {
    float trailRadius = 3.4f;
    if (const Ball* leadBall = firstActiveBall()) {
      trailRadius = leadBall->r;
    }
    for (size_t i = 0; i < trail.size(); ++i) {
      const size_t index = (trailHead + i) % trail.size();
      const auto& point = trail[index];
      if (!point.active) {
        continue;
      }

      const float alpha = static_cast<float>(i + 1) / static_cast<float>(trail.size());
      const int radius = std::max(1, static_cast<int>(trailRadius * alpha));
      canvas.fillCircle(static_cast<int>(point.x), static_cast<int>(point.y), radius,
                        mix565(rgb565(120, 196, 255), rgb565(255, 255, 255), alpha * 0.55f));
    }
  }

  for (const auto& ball : balls) {
    if (!ball.active) {
      continue;
    }

    const int x = static_cast<int>(ball.x);
    const int y = static_cast<int>(ball.y);
    const int r = static_cast<int>(ball.r);
    canvas.fillCircle(x, y, r + 2, rgb565(24, 28, 42));
    canvas.fillCircle(static_cast<int>(ball.x), static_cast<int>(ball.y),
                      static_cast<int>(ball.r + 1), rgb565(156, 176, 204));
    canvas.fillCircle(x, y, r, rgb565(236, 242, 255));
    canvas.fillCircle(x - 1, y - 1, std::max(1, r - 2), rgb565(255, 255, 255));
    canvas.fillCircle(x + 1, y + 1, 1, rgb565(148, 164, 198));
  }
}

void drawPowerUps() {
  for (const auto& power : powerUps) {
    if (!power.active) {
      continue;
    }

    uint16_t color = rgb565(128, 255, 180);
    char symbol = 'E';
    if (power.type == PowerType::Laser) {
      color = rgb565(255, 120, 96);
      symbol = 'L';
    } else if (power.type == PowerType::Slow) {
      color = rgb565(255, 228, 92);
      symbol = 'S';
    } else if (power.type == PowerType::MultiBall) {
      color = rgb565(150, 255, 120);
      symbol = 'M';
    }

    const int x = static_cast<int>(power.x) - 8;
    const int y = static_cast<int>(power.y) - 5;
    const uint16_t outline = rgb565(18, 20, 30);
    const uint16_t shell = rgb565(184, 190, 206);
    const uint16_t shellDark = rgb565(108, 114, 130);

    canvas.fillRoundRect(x, y, 16, 10, 3, outline);
    canvas.fillRoundRect(x + 1, y + 1, 14, 8, 3, shellDark);
    canvas.fillRoundRect(x + 2, y + 2, 12, 6, 2, color);
    canvas.fillRect(x + 1, y + 3, 2, 4, shell);
    canvas.fillRect(x + 13, y + 3, 2, 4, shell);
    canvas.drawFastHLine(x + 3, y + 2, 10, mix565(color, WHITE, 0.45f));
    canvas.drawFastHLine(x + 3, y + 7, 10, mix565(color, BLACK, 0.22f));

    canvas.setTextColor(BLACK, color);
    canvas.setCursor(static_cast<int>(power.x) - 3, static_cast<int>(power.y) - 3);
    canvas.print(symbol);
  }
}

void drawParticles() {
  for (const auto& particle : particles) {
    if (!particle.active || particle.maxLife <= 0.0f) {
      continue;
    }

    const float t = std::max(0.0f, particle.life / particle.maxLife);
    const uint16_t color = mix565(particle.color, WHITE, 0.22f + (1.0f - t) * 0.32f);
    const int x = static_cast<int>(particle.x);
    const int y = static_cast<int>(particle.y);
    const int size = t > 0.55f ? 2 : 1;
    canvas.fillRect(x, y, size, size, color);
  }
}

void drawLasers() {
  for (const auto& shot : lasers) {
    if (!shot.active) {
      continue;
    }

    const int x = static_cast<int>(shot.x);
    const int y = static_cast<int>(shot.y);
    canvas.drawFastVLine(x, y - 6, 11, rgb565(44, 18, 18));
    canvas.drawFastVLine(x, y - 5, 9, rgb565(255, 178, 72));
    canvas.drawFastVLine(x, y - 3, 5, rgb565(255, 238, 188));
    canvas.drawPixel(x - 1, y - 5, rgb565(255, 110, 72));
    canvas.drawPixel(x + 1, y - 5, rgb565(255, 110, 72));
    canvas.drawPixel(x - 1, y + 3, rgb565(255, 110, 72));
    canvas.drawPixel(x + 1, y + 3, rgb565(255, 110, 72));
  }
}

void drawHud() {
  const int x = 4;
  const int y = 3;
  const int w = 232;
  const int h = 9;
  const uint16_t outline = rgb565(18, 22, 34);
  const uint16_t shell = rgb565(126, 152, 198);
  const uint16_t fill = rgb565(34, 56, 108);

  canvas.fillRect(x, y, w, h, outline);
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, shell);
  canvas.fillRect(x + 2, y + 2, w - 4, h - 4, fill);
  canvas.drawFastHLine(x + 2, y + 2, w - 4, rgb565(186, 214, 255));
  canvas.drawFastHLine(x + 2, y + h - 3, w - 4, rgb565(22, 40, 84));

  for (int sx = x + 58; sx < x + w - 8; sx += 58) {
    canvas.drawFastVLine(sx, y + 2, h - 4, mix565(fill, WHITE, 0.12f));
  }

  canvas.setTextColor(WHITE, fill);
  char scoreText[20];
  char levelText[10];
  char livesText[12];
  std::snprintf(scoreText, sizeof(scoreText), "SCORE %05lu",
                static_cast<unsigned long>(score));
  std::snprintf(levelText, sizeof(levelText), "LV %u", level);
  std::snprintf(livesText, sizeof(livesText), "LIVES %u", lives);

  canvas.setCursor(9, 5);
  canvas.print(scoreText);

  const int levelX = (kScreenW - canvas.textWidth(levelText)) / 2;
  canvas.setCursor(std::max(0, levelX), 5);
  canvas.print(levelText);

  const int livesX = kScreenW - 8 - canvas.textWidth(livesText);
  canvas.setCursor(std::max(0, livesX), 5);
  canvas.print(livesText);

  const char* statusText = nullptr;
  if (millis() < laserUntilMs) {
    statusText = "LASER";
  } else if (millis() < slowUntilMs) {
    statusText = "SLOW";
  } else if (activeBallCount() > 1) {
    static char multiBallText[8];
    std::snprintf(multiBallText, sizeof(multiBallText), "x%u",
                  static_cast<unsigned>(activeBallCount()));
    statusText = multiBallText;
  }

  if (statusText != nullptr) {
    const int statusX = livesX - 8 - canvas.textWidth(statusText);
    const int scoreRight = 9 + canvas.textWidth(scoreText);
    if (statusX > scoreRight + 8) {
      canvas.setCursor(statusX, 5);
      canvas.print(statusText);
    }
  }
}

const char* difficultyLabel() {
  if (config.difficulty == 0) {
    return "EASY";
  }
  if (config.difficulty == 2) {
    return "HARD";
  }
  return "NORMAL";
}

const char* paddleLabel() {
  if (config.paddleSize == 0) {
    return "SLIM";
  }
  if (config.paddleSize == 2) {
    return "WIDE";
  }
  return "STD";
}

void drawCenteredText(const char* text, const int y, const uint16_t fg, const uint16_t bg) {
  canvas.setTextColor(fg, bg);
  const int x = (kScreenW - canvas.textWidth(text)) / 2;
  canvas.setCursor(std::max(0, x), y);
  canvas.print(text);
}

void drawFooterHint(const char* text) {
  drawSpritePanel(0, 123, kScreenW, 12, rgb565(36, 48, 76), rgb565(10, 18, 34),
                  rgb565(168, 188, 228), rgb565(8, 12, 22));
  drawCenteredText(text, 126, WHITE, rgb565(10, 18, 34));
}

void renderGyroBadge() {
  if (!gyroControlsEnabled) {
    return;
  }
  drawSpritePanel(150, 4, 86, 14, rgb565(102, 178, 120), rgb565(26, 82, 40));
  canvas.setTextColor(WHITE, rgb565(26, 82, 40));
  canvas.setCursor(158, 8);
  canvas.print("GYRO ON");
}

void renderTitleSplash() {
  canvas.fillScreen(BLACK);
  canvas.drawPng(title_png_start, titlePngSize(), 0, 0);
  renderGyroBadge();
  drawFooterHint("P START   G0 OPTIONS   V HELP");
}

void renderTitleScores() {
  drawMoireBackdrop();
  drawSpritePanel(18, 10, 204, 20, rgb565(112, 146, 212), rgb565(28, 58, 118));
  canvas.setTextColor(WHITE, rgb565(28, 58, 118));
  canvas.setTextSize(2);
  drawCenteredText("HIGHSCORES", 14, WHITE, rgb565(28, 58, 118));
  canvas.setTextSize(1);

  drawSpritePanel(22, 38, 196, 70, rgb565(126, 152, 198), rgb565(18, 28, 66));
  canvas.setTextColor(WHITE, rgb565(18, 28, 66));
  for (size_t i = 0; i < highScores.size(); ++i) {
    canvas.setCursor(52, 47 + static_cast<int>(i) * 12);
    canvas.printf("%u. %s", static_cast<unsigned>(i + 1), highScores[i].name);
    canvas.setCursor(146, 47 + static_cast<int>(i) * 12);
    canvas.printf("%5lu", static_cast<unsigned long>(highScores[i].score));
  }

  drawFooterHint("P START   G0 OPTIONS   V HELP");
  renderGyroBadge();
}

void renderTitle() {
  titleAnimMs = millis();
  if (millis() - stateTimerMs < kTitleScreenMs) {
    renderTitleSplash();
  } else {
    renderTitleScores();
  }
}

void renderKonamiReveal() {
  const uint32_t elapsed = millis() - stateTimerMs;
  const bool firstImage = elapsed < kKonamiFirstRevealMs;
  const uint8_t* start = firstImage ? konami_1_jpg_start : konami_2_jpg_start;
  const uint8_t* end = firstImage ? konami_1_jpg_end : konami_2_jpg_end;

  canvas.fillScreen(BLACK);
  canvas.drawJpg(start, binaryAssetSize(start, end), 0, 0);
}

void renderGyroOff() {
  canvas.fillScreen(BLACK);
  canvas.setTextColor(rgb565(255, 120, 120), BLACK);
  canvas.setTextSize(2);
  drawCenteredText("GYRO MODE OFF", 58, rgb565(255, 120, 120), BLACK);
  canvas.setTextSize(1);
}

void renderOptions() {
  drawBackdrop();
  drawSpritePanel(12, 6, 216, 123, rgb565(126, 152, 198), rgb565(26, 32, 74));

  canvas.setTextColor(WHITE, rgb565(26, 32, 74));
  canvas.setTextSize(2);
  drawCenteredText("OPTIONS", 14, WHITE, rgb565(26, 32, 74));
  canvas.setTextSize(1);
  drawCenteredText("A/D OPTION   P NEXT   G0 SAVE", 34, WHITE, rgb565(26, 32, 74));

  const std::array<const char*, kOptionCount> labels = {
      "SFX",
      "SFX VOL",
      "DIFFICULTY",
      "TRAILS",
      "PADDLE",
      "BG MODE",
      "LEFT KEY",
      "RIGHT KEY",
      "ACTION KEY",
      "MP3",
      "MUSIC VOL",
      "TRACK",
      "PLAYLIST",
      "MAIN TITLE",
  };

  const int visibleRows = 8;
  int startIndex = static_cast<int>(optionsIndex) - visibleRows / 2;
  if (startIndex < 0) {
    startIndex = 0;
  }
  if (startIndex > static_cast<int>(labels.size()) - visibleRows) {
    startIndex = static_cast<int>(labels.size()) - visibleRows;
  }
  if (startIndex < 0) {
    startIndex = 0;
  }

  for (int row = 0; row < visibleRows; ++row) {
    const size_t i = static_cast<size_t>(startIndex + row);
    const int y = 48 + row * 9;
    const bool selected = i == optionsIndex;
    const uint16_t rowBg = selected ? rgb565(66, 118, 196) : rgb565(26, 32, 74);
    const uint16_t rowText = selected ? WHITE : rgb565(200, 220, 255);
    if (selected) {
      drawSpritePanel(18, y - 2, 204, 10, rgb565(154, 176, 220), rgb565(66, 118, 196));
      canvas.setTextColor(WHITE, rowBg);
    } else {
      canvas.setTextColor(rowText, rowBg);
    }

    canvas.setCursor(24, y);
    canvas.print(labels[i]);
    switch (i) {
      case 0:
        canvas.setCursor(154, y);
        canvas.print(config.sfx ? "ON" : "OFF");
        break;
      case 1:
        canvas.setCursor(154, y);
        canvas.printf("%u%%", static_cast<unsigned>(config.volumeStep * 10));
        break;
      case 2:
        canvas.setCursor(154, y);
        canvas.print(difficultyLabel());
        break;
      case 3:
        canvas.setCursor(154, y);
        canvas.print(config.trails ? "ON" : "OFF");
        break;
      case 4:
        canvas.setCursor(154, y);
        canvas.print(paddleLabel());
        break;
      case 5:
        canvas.setCursor(154, y);
        canvas.print(config.randomBackground ? "RANDOM" : "CUSTOM");
        break;
      case 6:
        canvas.setCursor(154, y);
        canvas.print(bindingLabel(config.leftKey));
        break;
      case 7:
        canvas.setCursor(154, y);
        canvas.print(bindingLabel(config.rightKey));
        break;
      case 8:
        canvas.setCursor(154, y);
        canvas.print(bindingLabel(config.actionKey));
        break;
      case 9:
        canvas.setCursor(154, y);
        canvas.print(config.mp3Enabled ? "ON" : "OFF");
        break;
      case 10:
        canvas.setCursor(154, y);
        canvas.printf("%u%%", static_cast<unsigned>(config.mp3VolumeStep * 10));
        break;
      case 11:
        drawScrollingText(AudioTask::getCurrentTrackName(), 84, y, 100, rowText, rowBg, 0);
        canvas.setCursor(184, y);
        canvas.print(" ");
        canvas.print(AudioTask::getCurrentTrackType());
        break;
      case 12:
        canvas.setCursor(154, y);
        canvas.printf("%u SEL", static_cast<unsigned>(AudioTask::getPlaylistCopy().size()));
        break;
      case 13:
        canvas.setCursor(154, y);
        canvas.print("EXIT");
        break;
    }
  }

  if (startIndex > 0) {
    canvas.setCursor(206, 48);
    canvas.print("^");
  }
  if (startIndex + visibleRows < static_cast<int>(labels.size())) {
    canvas.setCursor(206, 111);
    canvas.print("v");
  }

  if (waitingForBinding) {
    drawSpritePanel(24, 106, 192, 16, rgb565(188, 154, 168), rgb565(92, 42, 54));
    canvas.setTextColor(WHITE, rgb565(92, 42, 54));
    canvas.setCursor(32, 110);
    canvas.print("Press new key...");
  }
}

void renderPlaylist() {
  drawBackdrop();
  drawSpritePanel(12, 6, 216, 123, rgb565(126, 152, 198), rgb565(20, 30, 68));
  canvas.setTextSize(2);
  drawCenteredText("PLAYLIST", 14, WHITE, rgb565(20, 30, 68));
  canvas.setTextSize(1);
  drawCenteredText("A/D TRACK   P TOGGLE   G0 SAVE", 34, WHITE, rgb565(20, 30, 68));

  if (availableTracks.empty()) {
    drawCenteredText("No MP3 files in /ADVnoid/music", 64, WHITE, rgb565(20, 30, 68));
    drawCenteredText("Copy tracks to SD and reopen", 76, WHITE, rgb565(20, 30, 68));
    return;
  }

  const int visibleRows = 7;
  int startIndex = playlistCursor - visibleRows / 2;
  if (startIndex < 0) {
    startIndex = 0;
  }
  if (startIndex > static_cast<int>(availableTracks.size()) - visibleRows) {
    startIndex = static_cast<int>(availableTracks.size()) - visibleRows;
  }
  if (startIndex < 0) {
    startIndex = 0;
  }

  for (int row = 0; row < visibleRows && startIndex + row < static_cast<int>(availableTracks.size());
       ++row) {
    const int idx = startIndex + row;
    const int y = 48 + row * 10;
    const bool selected = idx == playlistCursor;
    const bool enabled = playlistContains(availableTracks[static_cast<size_t>(idx)]);
    const uint16_t rowBg = selected ? rgb565(66, 118, 196) : rgb565(20, 30, 68);
    const uint16_t rowText =
        selected ? WHITE : (enabled ? rgb565(210, 255, 210) : rgb565(200, 220, 255));
    if (selected) {
      drawSpritePanel(18, y - 2, 204, 11, rgb565(154, 176, 220), rgb565(66, 118, 196));
      canvas.setTextColor(rowText, rowBg);
    } else {
      canvas.setTextColor(rowText, rowBg);
    }
    canvas.setCursor(24, y);
    canvas.print(enabled ? "[x]" : "[ ]");
    drawScrollingText(availableTracks[static_cast<size_t>(idx)], 52, y, 164, rowText, rowBg,
                      static_cast<uint32_t>(idx * 220));
  }

  drawCenteredText(("CURRENT TYPE: " + AudioTask::getCurrentTrackType()).c_str(), 108, WHITE,
                   rgb565(20, 30, 68));
  char summary[40];
  std::snprintf(summary, sizeof(summary), "%u TRACKS SELECTED",
                static_cast<unsigned>(playlistDraft.size()));
  drawCenteredText(summary, 118, WHITE, rgb565(20, 30, 68));
}

void renderPlaying() {
  drawLevelBackground();
  drawHud();
  drawBricks();
  drawParticles();
  drawPowerUps();
  drawLasers();
  drawPaddle();
  drawBalls();
}

void renderBackgroundSelect() {
  renderPlaying();
  drawSpritePanel(20, 104, 200, 19, rgb565(126, 152, 198), rgb565(18, 28, 66));
  canvas.setTextColor(WHITE, rgb565(18, 28, 66));
  char info[48];
  std::snprintf(info, sizeof(info), "BG %u/%u  %s",
                static_cast<unsigned>(config.backgroundIndex + 1),
                static_cast<unsigned>(kBackgroundAssets.size()),
                currentBackground().name);
  drawCenteredText(info, 109, WHITE, rgb565(18, 28, 66));
  drawFooterHint("A/D CHANGE   V/P/G0 BACK");
}

void renderPause() {
  renderPlaying();
  canvas.fillRect(0, 0, kScreenW, kScreenH, mix565(BLACK, rgb565(10, 18, 34), 0.55f));
  drawSpritePanel(52, 42, 136, 46, rgb565(148, 168, 210), rgb565(36, 46, 94));
  canvas.setTextColor(WHITE, rgb565(36, 46, 94));
  canvas.setTextSize(2);
  drawCenteredText("PAUSE", 52, WHITE, rgb565(36, 46, 94));
  canvas.setTextSize(1);
  drawCenteredText("B OR P RESUME", 74, WHITE, rgb565(36, 46, 94));
  drawFooterHint("B RESUME   G0 TITLE   V HELP");
}

void renderLevelClear() {
  renderPlaying();
  drawSpritePanel(52, 50, 136, 28, rgb565(162, 188, 228), rgb565(74, 134, 208));
  canvas.setTextColor(WHITE, rgb565(74, 134, 208));
  canvas.setCursor(82, 60);
  canvas.print("LEVEL CLEAR");
}

void renderGameOver() {
  drawMoireBackdrop();
  drawSpritePanel(30, 14, 180, 42, rgb565(192, 154, 166), rgb565(86, 24, 52));
  canvas.setTextColor(WHITE, rgb565(86, 24, 52));
  canvas.setTextSize(2);
  canvas.drawString("GAME OVER", 52, 26);
  canvas.setTextSize(1);
  canvas.setCursor(72, 44);
  canvas.printf("FINAL SCORE %05lu", static_cast<unsigned long>(score));

  drawSpritePanel(20, 62, 200, 54, rgb565(126, 152, 198), rgb565(18, 28, 66));
  canvas.setTextColor(WHITE, rgb565(18, 28, 66));
  drawCenteredText("HIGHSCORES", 70, WHITE, rgb565(18, 28, 66));
  for (size_t i = 0; i < highScores.size(); ++i) {
    const int col = i < 3 ? 0 : 1;
    const int row = i < 3 ? static_cast<int>(i) : static_cast<int>(i - 3);
    const int baseX = col == 0 ? 30 : 126;
    const int y = 84 + row * 10;
    canvas.setCursor(baseX, y);
    canvas.printf("%u.", static_cast<unsigned>(i + 1));
    canvas.setCursor(baseX + 14, y);
    canvas.print(highScores[i].name);
    canvas.setCursor(baseX + 36, y);
    canvas.printf("%5lu", static_cast<unsigned long>(highScores[i].score));
  }

  drawFooterHint("P TITLE   G0 OPTIONS   V HELP");
}

void renderNameEntry() {
  drawFireBackdrop();
  drawSpritePanel(22, 18, 196, 96, rgb565(126, 152, 198), rgb565(22, 34, 78));
  canvas.setTextColor(WHITE, rgb565(22, 34, 78));
  canvas.setTextSize(2);
  drawCenteredText("NEW HIGHSCORE", 28, WHITE, rgb565(22, 34, 78));
  canvas.setTextSize(1);
  drawCenteredText("ENTER YOUR NAME", 50, WHITE, rgb565(22, 34, 78));

  for (size_t i = 0; i < nameEntryChars.size(); ++i) {
    const int x = 68 + static_cast<int>(i) * 36;
    const bool selected = i == nameEntryIndex;
    drawSpritePanel(x, 68, 24, 24, selected ? rgb565(188, 198, 224) : rgb565(126, 152, 198),
                    selected ? rgb565(80, 136, 214) : rgb565(50, 72, 126));
    canvas.setTextColor(WHITE, selected ? rgb565(80, 136, 214) : rgb565(50, 72, 126));
    canvas.setTextSize(2);
    canvas.drawChar(displayNameChar(nameEntryChars[i]), x + 5, 73);
    canvas.setTextSize(1);
  }

  canvas.setTextColor(WHITE, rgb565(22, 34, 78));
  char controlsLine[40];
  std::snprintf(controlsLine, sizeof(controlsLine), "LEFT/RIGHT CHANGE   %s CONFIRM",
                bindingLabel(config.actionKey));
  drawCenteredText(controlsLine, 100, WHITE, rgb565(22, 34, 78));
}

void renderHelp() {
  drawBackdrop();
  drawSpritePanel(18, 14, 204, 108, rgb565(126, 152, 198), rgb565(18, 28, 66));
  canvas.setTextColor(WHITE, rgb565(18, 28, 66));
  canvas.setTextSize(2);
  drawCenteredText("HELP", 22, WHITE, rgb565(18, 28, 66));
  canvas.setTextSize(1);
  canvas.setCursor(30, 40);
  canvas.printf("%s  LEFT", bindingLabel(config.leftKey));
  canvas.setCursor(30, 51);
  canvas.printf("%s  RIGHT", bindingLabel(config.rightKey));
  canvas.setCursor(30, 62);
  canvas.printf("%s  START / FIRE", bindingLabel(config.actionKey));
  canvas.setCursor(30, 73);
  canvas.print("B  PAUSE");
  canvas.setCursor(30, 84);
  canvas.print(", /  TRACK");
  canvas.setCursor(30, 95);
  canvas.print("V  BG SELECT (CUSTOM)");
  canvas.setCursor(30, 106);
  canvas.print("G0   OPTIONS");
  canvas.setCursor(136, 106);
  canvas.print("V/P BACK");
}

void renderFrame() {
  if (canvasPtr == nullptr) {
    return;
  }
  canvas.fillScreen(BLACK);
  switch (gameState) {
    case GameState::Title:
      renderTitle();
      break;
    case GameState::Help:
      renderHelp();
      break;
    case GameState::Playing:
      renderPlaying();
      break;
    case GameState::BackgroundSelect:
      renderBackgroundSelect();
      break;
    case GameState::Pause:
      renderPause();
      break;
    case GameState::Options:
      renderOptions();
      break;
    case GameState::Playlist:
      renderPlaylist();
      break;
    case GameState::LevelClear:
      renderLevelClear();
      break;
    case GameState::NameEntry:
      renderNameEntry();
      break;
    case GameState::GameOver:
      renderGameOver();
      break;
    case GameState::KonamiReveal:
      renderKonamiReveal();
      break;
    case GameState::GyroOff:
      renderGyroOff();
      break;
  }
  canvas.pushSprite(0, 0);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  releaseLegacyI2S();
  M5Cardputer.begin(cfg, true);
  {
    auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate = 48000;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5Cardputer.Speaker.config(spk_cfg);
    M5Cardputer.Speaker.begin();
  }
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);

  canvasPtr = new M5Canvas(&M5Cardputer.Display);
  canvas.setColorDepth(16);
  canvas.createSprite(kScreenW, kScreenH);
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);

  backgroundCanvasPtr = new M5Canvas(&M5Cardputer.Display);
  backgroundCanvas.setColorDepth(16);
  backgroundCanvas.createSprite(kScreenW, kScreenH);
  backgroundCanvas.setTextFont(1);
  backgroundCanvas.setTextSize(1);
  backgroundCanvas.setTextWrap(false);

  randomSeed(micros());
  AudioTask::begin();
  initStorage();
  loadConfig();
  sanitizeBindings();
  loadHighScores();
  applyAudioConfig();
  AudioTask::loadPlaylist();
  syncAudioPlayback();
  initFireBackdrop();
  invalidateBackgroundCache();
  refreshBackgroundCache();
  resetGyroMotionState();

  paddle.x = (kScreenW - basePaddleWidth()) * 0.5f;
  paddle.w = basePaddleWidth();
  resetBallsOnPaddle();

  lastFrameMs = millis();
  stateTimerMs = millis();
}

void loop() {
  const InputState input = readInput();
  updateAudio();
  handleTrackShortcuts(input);

  const uint32_t now = millis();
  float dt = static_cast<float>(now - lastFrameMs) / 1000.0f;
  lastFrameMs = now;
  dt = clampf(dt, 0.001f, 0.025f);

  switch (gameState) {
    case GameState::Title:
      updateTitle(input);
      break;
    case GameState::Help:
      updateHelp(input);
      break;
    case GameState::BackgroundSelect:
      updateBackgroundSelect(input);
      break;
    case GameState::Pause:
      updatePause(input);
      break;
    case GameState::Options:
      updateOptions(input);
      break;
    case GameState::Playlist:
      updatePlaylist(input);
      break;
    case GameState::Playing:
      updatePlaying(dt, input);
      break;
    case GameState::LevelClear:
      updateLevelClear();
      break;
    case GameState::NameEntry:
      updateNameEntry(input);
      break;
    case GameState::GameOver:
      updateGameOver(input);
      break;
    case GameState::KonamiReveal:
      updateKonamiReveal();
      break;
    case GameState::GyroOff:
      updateGyroOff();
      break;
  }

  renderFrame();
  delay(6);
}
