#pragma once

#include <Arduino.h>

// Non-blocking driver for the JSN-SR04T ultrasonic sensor in its default
// Trig/Echo mode. trigger() sends one ping; the Echo pulse is timed by a pin
// interrupt, so nothing waits for it. Poll result() afterwards.
class JsnSr04t {
 public:
  enum Ping { PENDING, ECHO, NO_ECHO };

  JsnSr04t(int trigPin, int echoPin) : _trig(trigPin), _echo(echoPin) {}

  void begin();
  void trigger();
  // ECHO: echoUs is the Echo pulse width (round trip). NO_ECHO: nothing came
  // back within the timeout. PENDING: still waiting.
  Ping result(uint32_t &echoUs);

 private:
  static void IRAM_ATTR onEcho(void *arg);

  int _trig;
  int _echo;
  int64_t _triggeredUs = 0;
  volatile int64_t _riseUs = 0;
  volatile uint32_t _widthUs = 0;
  volatile bool _done = false;
};
