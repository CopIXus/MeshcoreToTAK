#pragma once

#include <Arduino.h>

class TakClient;

enum class TakUpdState : uint8_t { Idle, Checking, Installing, Rebooting, Failed };

// Firmware updates from the project's GitHub releases (or a .bin uploaded through the page).
// Network work runs in its own task so the mesh and web server keep running meanwhile.
class TakUpdate {
public:
  void begin(TakClient* client) { _client = client; }
  void loop();

  bool requestCheck();
  bool requestInstall(String& why);

  bool uploadBegin(size_t total, String& err);
  bool uploadWrite(const uint8_t* data, size_t len, size_t index, size_t total);
  bool uploadEnd(String& err);
  void uploadAbort();

  static const char* current();
  static const char* releaseUrl();
  const char* latest() const { return _latest; }
  bool available() const;
  TakUpdState state() const { return _state; }
  const char* stateName() const;
  int progress() const { return _progress; }
  const char* message() const { return _msg; }
  unsigned long checkedMs() const { return _checked_ms; }

private:
  enum class Job : uint8_t { Check, Install };

  TakClient* _client = nullptr;
  volatile TakUpdState _state = TakUpdState::Idle;
  volatile bool _running = false;
  volatile bool _uploading = false;
  volatile int _progress = 0;
  Job _job = Job::Check;
  char _latest[16] = {0};
  char _md5[33] = {0};
  uint32_t _size = 0;
  char _msg[96] = {0};
  unsigned long _checked_ms = 0;
  unsigned long _next_check_ms = 10UL * 60000UL;  // let the TAK links settle after boot
  unsigned long _reboot_at = 0;

  bool start(Job job);
  static void taskEntry(void* arg);
  bool doCheck(String& err);
  bool doInstall(String& err);
  void freeTakLinks();
  void setMsg(TakUpdState s, const char* fmt, ...);
};

extern TakUpdate tak_update;
