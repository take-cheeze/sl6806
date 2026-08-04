/*
 * Stream.h - Arduino-compatible Stream base class.
 */
#ifndef SL6806_STREAM_H
#define SL6806_STREAM_H

#include "Print.h"

class Stream : public Print {
public:
    Stream() : timeout_(1000) {}

    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    void          setTimeout(unsigned long ms) { timeout_ = ms; }
    unsigned long getTimeout() const { return timeout_; }

    bool   find(const char *target);
    size_t readBytes(char *buffer, size_t length);
    size_t readBytesUntil(char terminator, char *buffer, size_t length);
    String readString();
    String readStringUntil(char terminator);
    long   parseInt();
    float  parseFloat();

protected:
    /* Blocking read honouring the timeout; -1 if it expires. */
    int timedRead();
    int timedPeek();
    unsigned long timeout_;
};

#endif /* SL6806_STREAM_H */
