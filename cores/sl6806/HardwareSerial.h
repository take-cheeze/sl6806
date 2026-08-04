/*
 * HardwareSerial.h - Serial for SL6806.
 *
 * Serial is NOT a UART here. It is the RAM ring buffer described in
 * sl6806_console.h, polled by tools/sl6806-monitor over the same USB link
 * that loaded the sketch. That is the only output channel known to work on
 * this chip today.
 *
 * begin() takes a baud rate for source compatibility with Arduino sketches
 * and ignores it - there is no wire whose speed it could set.
 */
#ifndef SL6806_HARDWARESERIAL_H
#define SL6806_HARDWARESERIAL_H

#include "Stream.h"

class HardwareSerial : public Stream {
public:
    void begin(unsigned long baud = 0);
    void end();

    virtual int available();
    virtual int read();
    virtual int peek();
    virtual void flush();
    virtual size_t write(uint8_t c);
    virtual size_t write(const uint8_t *buf, size_t size);
    using Print::write;

    virtual int availableForWrite();

    explicit operator bool() const { return true; }

    /*
     * Also send every byte to the boot ROM's printf (ROM 0x0640).
     *
     * Where that ends up is not established - most likely a hardware UART on
     * pins we have not identified. Off by default because a call into the ROM
     * costs far more than a ring-buffer write, and may block if the ROM's
     * output path is not ready. Turn it on if you have found the debug UART
     * and want to scope it.
     */
    void setRomEcho(bool on) { rom_echo_ = on; }

private:
    bool rom_echo_ = false;
};

extern HardwareSerial Serial;

#endif /* SL6806_HARDWARESERIAL_H */
