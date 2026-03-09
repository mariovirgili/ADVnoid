#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

extern SemaphoreHandle_t sdMutex;

namespace AudioTask {

extern bool mp3Enabled;
extern uint8_t mp3Volume;

void begin();
void playMP3(const char* path);
void stop();
bool isPlaying();
void setMP3Volume(uint8_t vol);

void loadPlaylist();
void startPlaylist();
void savePlaylist(const std::vector<String>& newPlaylist);
std::vector<String> getPlaylistCopy();

void nextTrack();
void prevTrack();
String getCurrentTrackName();
String getCurrentTrackType();

}  // namespace AudioTask
