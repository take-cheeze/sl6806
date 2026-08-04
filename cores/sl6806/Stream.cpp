#include "Stream.h"
#include "wiring_time.h"

int Stream::timedRead()
{
    unsigned long start = millis();

    do {
        int c = read();
        if (c >= 0)
            return c;
        yield();
    } while (millis() - start < timeout_);

    return -1;
}

int Stream::timedPeek()
{
    unsigned long start = millis();

    do {
        int c = peek();
        if (c >= 0)
            return c;
        yield();
    } while (millis() - start < timeout_);

    return -1;
}

bool Stream::find(const char *target)
{
    size_t index = 0;

    if (!target || !*target)
        return true;

    while (true) {
        int c = timedRead();
        if (c < 0)
            return false;

        if (c == target[index]) {
            if (!target[++index])
                return true;
        } else {
            /* Restart, but do not drop a character that itself begins a
             * fresh match. */
            index = (c == target[0]) ? 1 : 0;
            if (index && !target[1])
                return true;
        }
    }
}

size_t Stream::readBytes(char *buffer, size_t length)
{
    size_t count = 0;

    while (count < length) {
        int c = timedRead();
        if (c < 0)
            break;
        buffer[count++] = static_cast<char>(c);
    }
    return count;
}

size_t Stream::readBytesUntil(char terminator, char *buffer, size_t length)
{
    size_t count = 0;

    while (count < length) {
        int c = timedRead();
        if (c < 0 || c == terminator)
            break;
        buffer[count++] = static_cast<char>(c);
    }
    return count;
}

String Stream::readString()
{
    String out;
    int c;

    while ((c = timedRead()) >= 0)
        out += static_cast<char>(c);
    return out;
}

String Stream::readStringUntil(char terminator)
{
    String out;
    int c;

    while ((c = timedRead()) >= 0 && c != terminator)
        out += static_cast<char>(c);
    return out;
}

long Stream::parseInt()
{
    bool negative = false;
    long value = 0;
    int c;

    /* Skip anything that cannot start a number. */
    do {
        c = timedPeek();
        if (c < 0)
            return 0;
        if (c == '-' || (c >= '0' && c <= '9'))
            break;
        read();
    } while (true);

    if (c == '-') {
        negative = true;
        read();
    }

    while (true) {
        c = timedPeek();
        if (c < '0' || c > '9')
            break;
        value = value * 10 + (c - '0');
        read();
    }

    return negative ? -value : value;
}

float Stream::parseFloat()
{
    bool negative = false, fraction = false;
    double value = 0.0, scale = 1.0;
    int c;

    do {
        c = timedPeek();
        if (c < 0)
            return 0.0f;
        if (c == '-' || (c >= '0' && c <= '9'))
            break;
        read();
    } while (true);

    if (c == '-') {
        negative = true;
        read();
    }

    while (true) {
        c = timedPeek();
        if (c >= '0' && c <= '9') {
            value = value * 10.0 + (c - '0');
            if (fraction)
                scale *= 10.0;
        } else if (c == '.' && !fraction) {
            fraction = true;
        } else {
            break;
        }
        read();
    }

    value /= scale;
    return static_cast<float>(negative ? -value : value);
}
