/*
 * Print.h - Arduino-compatible Print base class.
 */
#ifndef SL6806_PRINT_H
#define SL6806_PRINT_H

#include <stdint.h>
#include <stddef.h>
#include "WString.h"

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print {
public:
    Print() : write_error_(0) {}
    virtual ~Print() {}

    /* The one method a subclass must implement. */
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t size);

    size_t write(const char *s);
    size_t write(const char *buf, size_t size) {
        return write(reinterpret_cast<const uint8_t *>(buf), size);
    }

    virtual int availableForWrite() { return 0; }
    virtual void flush() {}

    int  getWriteError() const { return write_error_; }
    void clearWriteError()     { write_error_ = 0; }

    size_t print(const char *s);
    size_t print(char c);
    size_t print(const String &s);
    size_t print(int n, int base = DEC);
    size_t print(unsigned int n, int base = DEC);
    size_t print(long n, int base = DEC);
    size_t print(unsigned long n, int base = DEC);
    size_t print(long long n, int base = DEC);
    size_t print(unsigned long long n, int base = DEC);
    size_t print(double n, int digits = 2);

    size_t println();
    size_t println(const char *s);
    size_t println(char c);
    size_t println(const String &s);
    size_t println(int n, int base = DEC);
    size_t println(unsigned int n, int base = DEC);
    size_t println(long n, int base = DEC);
    size_t println(unsigned long n, int base = DEC);
    size_t println(long long n, int base = DEC);
    size_t println(unsigned long long n, int base = DEC);
    size_t println(double n, int digits = 2);

    /* printf() over the same sink. Bounded to 256 bytes per call. */
    size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

protected:
    void setWriteError(int err = 1) { write_error_ = err; }

private:
    size_t printNumber(unsigned long long n, int base, bool negative);
    size_t printFloat(double n, int digits);
    int write_error_;
};

#endif /* SL6806_PRINT_H */
