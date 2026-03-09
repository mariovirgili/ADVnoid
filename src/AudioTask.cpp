#include "AudioTask.h"

#include <M5Cardputer.h>
#include <SD.h>

#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

SemaphoreHandle_t sdMutex = nullptr;

namespace AudioTask {
bool mp3Enabled = false;
uint8_t mp3Volume = 128;
}  // namespace AudioTask

namespace {

constexpr char kPlaylistFile[] = "/ADVnoid/playlist.txt";
constexpr char kMusicDir[] = "/ADVnoid/music/";
constexpr uint8_t kMp3VirtualChannel = 1;

float mp3Gain = 0.5f;
volatile int currentTrackChannels = 0;

class AudioOutputM5Speaker : public AudioOutput {
 public:
  explicit AudioOutputM5Speaker(m5::Speaker_Class* speaker) : speaker_(speaker) {}

  bool begin() override { return true; }

  bool SetRate(int hz) override {
    sampleRate_ = hz;
    return true;
  }

  bool SetChannels(int chan) override {
    sourceChannels_ = chan;
    currentTrackChannels = chan;
    return AudioOutput::SetChannels(chan);
  }

  bool ConsumeSample(int16_t sample[2]) override {
    if (bufferIndex_ + 1 < kBufferSamples) {
      const int16_t monoOrLeft = sample[0];
      const int16_t rightInput = (sourceChannels_ == 1) ? monoOrLeft : sample[1];
      int32_t left = static_cast<int32_t>(monoOrLeft * mp3Gain);
      int32_t right = static_cast<int32_t>(rightInput * mp3Gain);
      left = std::max<int32_t>(-32768, std::min<int32_t>(32767, left));
      right = std::max<int32_t>(-32768, std::min<int32_t>(32767, right));
      buffers_[bufferSet_][bufferIndex_++] = static_cast<int16_t>(left);
      buffers_[bufferSet_][bufferIndex_++] = static_cast<int16_t>(right);
      return true;
    }
    flush();
    return false;
  }

  bool stop() override {
    flush();
    speaker_->stop(kMp3VirtualChannel);
    return true;
  }

 private:
  static constexpr size_t kBufferSamples = 1536;

  void flush() {
    if (bufferIndex_ == 0) {
      return;
    }

    speaker_->playRaw(buffers_[bufferSet_], bufferIndex_, sampleRate_, true, 1,
                      kMp3VirtualChannel);
    bufferSet_ = (bufferSet_ + 1) % 3;
    bufferIndex_ = 0;
  }

  m5::Speaker_Class* speaker_;
  int16_t buffers_[3][kBufferSamples] = {};
  size_t bufferIndex_ = 0;
  size_t bufferSet_ = 0;
  int sampleRate_ = 44100;
  int sourceChannels_ = 2;
};

AudioGeneratorMP3* mp3 = nullptr;
AudioFileSourceSD* file = nullptr;
AudioOutputM5Speaker* output = nullptr;
TaskHandle_t audioTaskHandle = nullptr;

String currentFile;
volatile bool playRequested = false;
volatile bool stopRequested = false;

std::vector<String> playlist;
int currentTrackIdx = -1;

void audioLoop(void*) {
  while (true) {
    if (stopRequested) {
      if (mp3 && mp3->isRunning()) {
        mp3->stop();
      }
      if (file && file->isOpen()) {
        file->close();
      }
      stopRequested = false;
    }

    if (playRequested) {
      if (mp3 && mp3->isRunning()) {
        mp3->stop();
      }
      if (file && file->isOpen()) {
        file->close();
      }
      if (sdMutex == nullptr || xSemaphoreTake(sdMutex, portMAX_DELAY)) {
        file->open(currentFile.c_str());
        if (sdMutex != nullptr) {
          xSemaphoreGive(sdMutex);
        }
        if (file->isOpen()) {
          mp3->begin(file, output);
        }
      }
      playRequested = false;
    }

    if (mp3 && mp3->isRunning()) {
      bool loopResult = false;
      bool didLoop = false;
      if (sdMutex == nullptr || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10))) {
        didLoop = true;
        loopResult = mp3->loop();
        if (sdMutex != nullptr) {
          xSemaphoreGive(sdMutex);
        }
      }

      if (!didLoop) {
        vTaskDelay(1);
        continue;
      }

      if (!loopResult) {
        mp3->stop();
        if (file && file->isOpen()) {
          file->close();
        }

        if (AudioTask::mp3Enabled && !playlist.empty() && !stopRequested) {
          currentTrackIdx = (currentTrackIdx + 1) % static_cast<int>(playlist.size());
          currentFile = String(kMusicDir) + playlist[static_cast<size_t>(currentTrackIdx)];
          playRequested = true;
        }
      } else {
        static uint8_t yieldCounter = 0;
        if (++yieldCounter > 3) {
          vTaskDelay(1);
          yieldCounter = 0;
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

}  // namespace

namespace AudioTask {

void begin() {
  if (sdMutex == nullptr) {
    sdMutex = xSemaphoreCreateMutex();
  }
  if (output == nullptr) {
    output = new AudioOutputM5Speaker(&M5Cardputer.Speaker);
  }
  if (mp3 == nullptr) {
    mp3 = new AudioGeneratorMP3();
  }
  if (file == nullptr) {
    file = new AudioFileSourceSD();
  }
  if (audioTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(audioLoop, "AudioTask", 8192, nullptr, 1, &audioTaskHandle, 0);
  }
}

void playMP3(const char* path) {
  stopRequested = true;
  delay(10);
  currentFile = String(path);
  currentTrackChannels = 0;
  playRequested = true;
}

void stop() {
  playRequested = false;
  stopRequested = true;
  currentTrackChannels = 0;
  delay(20);
  M5Cardputer.Speaker.stop();
}

bool isPlaying() {
  return mp3 != nullptr && mp3->isRunning();
}

void setMP3Volume(uint8_t vol) {
  mp3Volume = vol;
  mp3Gain = static_cast<float>(vol) / 255.0f;
}

void loadPlaylist() {
  playlist.clear();
  if (sdMutex == nullptr || xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    if (SD.exists(kPlaylistFile)) {
      File playlistFile = SD.open(kPlaylistFile, FILE_READ);
      if (playlistFile) {
        while (playlistFile.available()) {
          String line = playlistFile.readStringUntil('\n');
          line.trim();
          if (!line.isEmpty()) {
            playlist.push_back(line);
          }
        }
        playlistFile.close();
      }
    }
    if (sdMutex != nullptr) {
      xSemaphoreGive(sdMutex);
    }
  }
}

void startPlaylist() {
  loadPlaylist();
  if (!mp3Enabled || playlist.empty()) {
    stop();
    return;
  }
  currentTrackIdx = 0;
  playMP3((String(kMusicDir) + playlist[0]).c_str());
}

void savePlaylist(const std::vector<String>& newPlaylist) {
  if (sdMutex == nullptr || xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    if (!SD.exists("/ADVnoid")) {
      SD.mkdir("/ADVnoid");
    }
    if (SD.exists(kPlaylistFile)) {
      SD.remove(kPlaylistFile);
    }
    File playlistFile = SD.open(kPlaylistFile, FILE_WRITE);
    if (playlistFile) {
      for (const auto& track : newPlaylist) {
        playlistFile.println(track);
      }
      playlistFile.close();
    }
    if (sdMutex != nullptr) {
      xSemaphoreGive(sdMutex);
    }
  }
  loadPlaylist();
}

std::vector<String> getPlaylistCopy() {
  return playlist;
}

void nextTrack() {
  if (playlist.empty()) {
    return;
  }
  currentTrackIdx = (currentTrackIdx + 1) % static_cast<int>(playlist.size());
  playMP3((String(kMusicDir) + playlist[static_cast<size_t>(currentTrackIdx)]).c_str());
}

void prevTrack() {
  if (playlist.empty()) {
    return;
  }
  currentTrackIdx--;
  if (currentTrackIdx < 0) {
    currentTrackIdx = static_cast<int>(playlist.size()) - 1;
  }
  playMP3((String(kMusicDir) + playlist[static_cast<size_t>(currentTrackIdx)]).c_str());
}

String getCurrentTrackName() {
  if (playlist.empty() || currentTrackIdx < 0 ||
      currentTrackIdx >= static_cast<int>(playlist.size())) {
    return "None";
  }
  return playlist[static_cast<size_t>(currentTrackIdx)];
}

String getCurrentTrackType() {
  if (currentTrackChannels == 1) {
    return "MONO";
  }
  if (currentTrackChannels >= 2) {
    return "STEREO";
  }
  return "UNKNOWN";
}

}  // namespace AudioTask
