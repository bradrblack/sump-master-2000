#include "jsn_sr04t.h"

#include <driver/gpio.h>
#include <esp_timer.h>

namespace {
// Longer than any real echo (the sensor's ~4.5 m range is ~26 ms round trip);
// some boards hold Echo high for ~40-60 ms when nothing comes back.
const int64_t PING_TIMEOUT_US = 70000;
}  // namespace

void JsnSr04t::begin() {
  pinMode(_trig, OUTPUT);
  digitalWrite(_trig, LOW);
  pinMode(_echo, INPUT);
  attachInterruptArg(_echo, onEcho, this, CHANGE);
}

void IRAM_ATTR JsnSr04t::onEcho(void *arg) {
  JsnSr04t *self = static_cast<JsnSr04t *>(arg);
  int64_t now = esp_timer_get_time();
  if (gpio_get_level((gpio_num_t)self->_echo)) {
    self->_riseUs = now;
  } else if (self->_riseUs && !self->_done) {
    self->_widthUs = (uint32_t)(now - self->_riseUs);
    self->_done = true;
  }
}

void JsnSr04t::trigger() {
  _done = false;
  _riseUs = 0;
  // The JSN-SR04T wants a longer trigger pulse than the HC-SR04's 10 us.
  digitalWrite(_trig, HIGH);
  delayMicroseconds(20);
  digitalWrite(_trig, LOW);
  _triggeredUs = esp_timer_get_time();
}

JsnSr04t::Ping JsnSr04t::result(uint32_t &echoUs) {
  if (_done) {
    echoUs = _widthUs;
    return ECHO;
  }
  return esp_timer_get_time() - _triggeredUs > PING_TIMEOUT_US ? NO_ECHO : PENDING;
}
