#include "Print.h"
#include <stdarg.h>
#include <stdio.h>

size_t Print::write(const uint8_t *buf, size_t size)
{
    size_t n = 0;

    while (size--) {
        if (write(*buf++))
            n++;
        else
            break;
    }
    return n;
}

size_t Print::write(const char *s)
{
    size_t n = 0;

    if (!s)
        return 0;
    while (s[n])
        n++;
    return write(reinterpret_cast<const uint8_t *>(s), n);
}

size_t Print::print(const char *s)      { return write(s); }
size_t Print::print(char c)             { return write(static_cast<uint8_t>(c)); }
size_t Print::print(const String &s)    { return write(s.c_str(), s.length()); }

size_t Print::print(int n, int base)
{
    return print(static_cast<long>(n), base);
}

size_t Print::print(unsigned int n, int base)
{
    return print(static_cast<unsigned long>(n), base);
}

size_t Print::print(long n, int base)
{
    return print(static_cast<long long>(n), base);
}

size_t Print::print(unsigned long n, int base)
{
    return print(static_cast<unsigned long long>(n), base);
}

size_t Print::print(long long n, int base)
{
    if (base == DEC && n < 0)
        return printNumber(static_cast<unsigned long long>(-n), DEC, true);
    return printNumber(static_cast<unsigned long long>(n), base, false);
}

size_t Print::print(unsigned long long n, int base)
{
    return printNumber(n, base, false);
}

size_t Print::print(double n, int digits)
{
    return printFloat(n, digits);
}

size_t Print::println()                        { return write("\r\n"); }
size_t Print::println(const char *s)           { return print(s) + println(); }
size_t Print::println(char c)                  { return print(c) + println(); }
size_t Print::println(const String &s)         { return print(s) + println(); }
size_t Print::println(int n, int base)         { return print(n, base) + println(); }
size_t Print::println(unsigned int n, int b)   { return print(n, b) + println(); }
size_t Print::println(long n, int base)        { return print(n, base) + println(); }
size_t Print::println(unsigned long n, int b)  { return print(n, b) + println(); }
size_t Print::println(long long n, int base)   { return print(n, base) + println(); }
size_t Print::println(unsigned long long n, int b) { return print(n, b) + println(); }
size_t Print::println(double n, int digits)    { return print(n, digits) + println(); }

size_t Print::printNumber(unsigned long long n, int base, bool negative)
{
    char buf[8 * sizeof(unsigned long long) + 2];
    char *p = buf + sizeof(buf);
    size_t len = 0;

    if (base < 2)
        base = 10;

    *--p = '\0';
    do {
        unsigned digit = static_cast<unsigned>(n % base);
        n /= base;
        *--p = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
    } while (n);

    if (negative)
        *--p = '-';

    while (p[len])
        len++;
    return write(reinterpret_cast<const uint8_t *>(p), len);
}

size_t Print::printFloat(double n, int digits)
{
    size_t count = 0;

    /* __builtin_isnan/isinf avoid pulling in math.h on a freestanding build. */
    if (__builtin_isnan(n))
        return write("nan");
    if (__builtin_isinf(n))
        return write(n < 0 ? "-inf" : "inf");

    if (n < 0.0) {
        count += write(static_cast<uint8_t>('-'));
        n = -n;
    }

    /* Round at the requested precision before splitting the parts, so that
     * 1.999 with 2 digits prints "2.00" rather than "1.100". */
    double rounding = 0.5;
    for (int i = 0; i < digits; i++)
        rounding /= 10.0;
    n += rounding;

    unsigned long long ipart = static_cast<unsigned long long>(n);
    count += printNumber(ipart, 10, false);

    if (digits > 0)
        count += write(static_cast<uint8_t>('.'));

    double frac = n - static_cast<double>(ipart);
    while (digits-- > 0) {
        frac *= 10.0;
        unsigned d = static_cast<unsigned>(frac);
        count += write(static_cast<uint8_t>('0' + d));
        frac -= d;
    }
    return count;
}

size_t Print::printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0)
        return 0;
    if (static_cast<size_t>(n) >= sizeof(buf))
        n = sizeof(buf) - 1;   /* truncated */
    return write(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n));
}
