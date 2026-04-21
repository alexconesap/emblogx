#pragma once

#include <stdint.h>
#include <stdio.h>

#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2

inline void pinMode(int pin, int mode) {
    (void)pin;
    (void)mode;
}
inline void digitalWrite(int pin, int value) {
    (void)pin;
    (void)value;
}
inline int digitalRead(int pin) {
    (void)pin;
    return 1;
}
inline void delay(unsigned long ms) {
    (void)ms;
}
inline unsigned long millis() {
    return 0;
}

using String = std::string;
