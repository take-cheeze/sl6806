#include "WString.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void String::init()
{
    buf_ = 0;
    cap_ = 0;
    len_ = 0;
}

void String::invalidate()
{
    if (buf_)
        free(buf_);
    init();
}

String::String(const char *s)
{
    init();
    if (s)
        copy(s, strlen(s));
}

String::String(const String &s)
{
    init();
    if (s.buf_)
        copy(s.buf_, s.len_);
}

String::String(char c)
{
    char b[2] = { c, '\0' };
    init();
    copy(b, 1);
}

String::String(int v, unsigned char base)
{
    init();
    fromNumber(v < 0 && base == 10 ? -(long)v : (unsigned long)v, base, v < 0 && base == 10);
}

String::String(unsigned int v, unsigned char base)
{
    init();
    fromNumber(v, base, false);
}

String::String(long v, unsigned char base)
{
    init();
    fromNumber(v < 0 && base == 10 ? -v : (unsigned long)v, base, v < 0 && base == 10);
}

String::String(unsigned long v, unsigned char base)
{
    init();
    fromNumber(v, base, false);
}

String::String(float v, unsigned char decimals)
{
    char b[40];
    init();
    snprintf(b, sizeof(b), "%.*f", decimals, (double)v);
    copy(b, strlen(b));
}

String::String(double v, unsigned char decimals)
{
    char b[40];
    init();
    snprintf(b, sizeof(b), "%.*f", decimals, v);
    copy(b, strlen(b));
}

String::~String()
{
    if (buf_)
        free(buf_);
}

String &String::fromNumber(unsigned long v, unsigned char base, bool neg)
{
    char b[8 * sizeof(unsigned long) + 2];
    char *p = b + sizeof(b) - 1;

    if (base < 2)
        base = 10;
    *p = '\0';
    do {
        unsigned d = (unsigned)(v % base);
        v /= base;
        *--p = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    } while (v);
    if (neg)
        *--p = '-';

    copy(p, strlen(p));
    return *this;
}

bool String::changeBuffer(unsigned int maxlen)
{
    /* Round up to a power of two so repeated concat is amortised O(n). */
    unsigned int want = 8;

    while (want < maxlen + 1)
        want <<= 1;

    if (buf_ && cap_ >= want)
        return true;

    char *nb = (char *)realloc(buf_, want);
    if (!nb)
        return false;

    buf_ = nb;
    cap_ = want;
    return true;
}

bool String::copy(const char *s, unsigned int len)
{
    if (!changeBuffer(len)) {
        invalidate();
        return false;
    }
    memcpy(buf_, s, len);
    buf_[len] = '\0';
    len_ = len;
    return true;
}

bool String::reserve(unsigned int size)
{
    if (buf_ && cap_ >= size + 1)
        return true;
    if (!changeBuffer(size))
        return false;
    if (len_ == 0)
        buf_[0] = '\0';
    return true;
}

String &String::operator=(const String &rhs)
{
    if (this == &rhs)
        return *this;
    if (rhs.buf_)
        copy(rhs.buf_, rhs.len_);
    else
        invalidate();
    return *this;
}

String &String::operator=(const char *s)
{
    if (s)
        copy(s, strlen(s));
    else
        invalidate();
    return *this;
}

bool String::concat(const char *s)
{
    unsigned int add, total;

    if (!s)
        return false;
    add = strlen(s);
    if (add == 0)
        return true;

    total = len_ + add;
    if (!changeBuffer(total))
        return false;

    memcpy(buf_ + len_, s, add);
    buf_[total] = '\0';
    len_ = total;
    return true;
}

bool String::concat(const String &s)
{
    return s.buf_ ? concat(s.buf_) : true;
}

bool String::concat(char c)
{
    char b[2] = { c, '\0' };
    return concat(b);
}

bool String::concat(long v)
{
    char b[8 * sizeof(long) + 2];
    snprintf(b, sizeof(b), "%ld", v);
    return concat(b);
}

bool String::concat(unsigned long v)
{
    char b[8 * sizeof(unsigned long) + 2];
    snprintf(b, sizeof(b), "%lu", v);
    return concat(b);
}

bool String::concat(double v)
{
    char b[40];
    snprintf(b, sizeof(b), "%.2f", v);
    return concat(b);
}

int String::compareTo(const String &s) const
{
    if (!buf_ || !s.buf_)
        return (buf_ ? 1 : 0) - (s.buf_ ? 1 : 0);
    return strcmp(buf_, s.buf_);
}

bool String::equals(const String &s) const
{
    return len_ == s.len_ && compareTo(s) == 0;
}

bool String::equals(const char *s) const
{
    if (!buf_)
        return !s || !*s;
    if (!s)
        return len_ == 0;
    return strcmp(buf_, s) == 0;
}

bool String::equalsIgnoreCase(const String &s) const
{
    if (len_ != s.len_)
        return false;
    if (!buf_ || !s.buf_)
        return buf_ == s.buf_;
    for (unsigned int i = 0; i < len_; i++) {
        char a = buf_[i], b = s.buf_[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return false;
    }
    return true;
}

char String::operator[](unsigned int index) const
{
    if (!buf_ || index >= len_)
        return 0;
    return buf_[index];
}

void String::setCharAt(unsigned int index, char c)
{
    if (buf_ && index < len_)
        buf_[index] = c;
}

bool String::startsWith(const String &prefix) const
{
    if (len_ < prefix.len_ || !buf_ || !prefix.buf_)
        return false;
    return memcmp(buf_, prefix.buf_, prefix.len_) == 0;
}

bool String::endsWith(const String &suffix) const
{
    if (len_ < suffix.len_ || !buf_ || !suffix.buf_)
        return false;
    return memcmp(buf_ + len_ - suffix.len_, suffix.buf_, suffix.len_) == 0;
}

int String::indexOf(char c, unsigned int from) const
{
    if (!buf_ || from >= len_)
        return -1;
    const char *p = strchr(buf_ + from, c);
    return p ? (int)(p - buf_) : -1;
}

int String::indexOf(const String &s, unsigned int from) const
{
    if (!buf_ || !s.buf_ || from >= len_)
        return -1;
    const char *p = strstr(buf_ + from, s.buf_);
    return p ? (int)(p - buf_) : -1;
}

int String::lastIndexOf(char c) const
{
    if (!buf_)
        return -1;
    const char *p = strrchr(buf_, c);
    return p ? (int)(p - buf_) : -1;
}

String String::substring(unsigned int from, unsigned int to) const
{
    String out;

    if (!buf_ || from >= len_)
        return out;
    if (to > len_)
        to = len_;
    if (to <= from)
        return out;

    out.copy(buf_ + from, to - from);
    return out;
}

void String::replace(char find, char rep)
{
    if (!buf_)
        return;
    for (unsigned int i = 0; i < len_; i++)
        if (buf_[i] == find)
            buf_[i] = rep;
}

void String::remove(unsigned int index, unsigned int count)
{
    if (!buf_ || index >= len_)
        return;
    if (count > len_ - index)
        count = len_ - index;
    memmove(buf_ + index, buf_ + index + count, len_ - index - count);
    len_ -= count;
    buf_[len_] = '\0';
}

void String::toLowerCase()
{
    if (!buf_)
        return;
    for (unsigned int i = 0; i < len_; i++)
        if (buf_[i] >= 'A' && buf_[i] <= 'Z')
            buf_[i] += 32;
}

void String::toUpperCase()
{
    if (!buf_)
        return;
    for (unsigned int i = 0; i < len_; i++)
        if (buf_[i] >= 'a' && buf_[i] <= 'z')
            buf_[i] -= 32;
}

void String::trim()
{
    unsigned int b = 0, e;

    if (!buf_ || len_ == 0)
        return;
    e = len_;
    while (b < e && (unsigned char)buf_[b] <= ' ')
        b++;
    while (e > b && (unsigned char)buf_[e - 1] <= ' ')
        e--;

    len_ = e - b;
    if (b)
        memmove(buf_, buf_ + b, len_);
    buf_[len_] = '\0';
}

long   String::toInt()    const { return buf_ ? strtol(buf_, 0, 10) : 0; }
float  String::toFloat()  const { return (float)toDouble(); }
double String::toDouble() const { return buf_ ? strtod(buf_, 0) : 0.0; }
