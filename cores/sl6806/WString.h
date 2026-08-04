/*
 * WString.h - Arduino-compatible String.
 *
 * Heap-backed, with the same "never throws, fails to an empty/invalid string"
 * behaviour as the Arduino original. Capacity grows in powers of two.
 */
#ifndef SL6806_WSTRING_H
#define SL6806_WSTRING_H

#include <stdint.h>
#include <stddef.h>

class String {
public:
    String(const char *s = 0);
    String(const String &s);
    explicit String(char c);
    explicit String(int v, unsigned char base = 10);
    explicit String(unsigned int v, unsigned char base = 10);
    explicit String(long v, unsigned char base = 10);
    explicit String(unsigned long v, unsigned char base = 10);
    explicit String(float v, unsigned char decimals = 2);
    explicit String(double v, unsigned char decimals = 2);
    ~String();

    String &operator=(const String &rhs);
    String &operator=(const char *s);

    /* True if the last allocation succeeded. */
    explicit operator bool() const { return buf_ != 0; }

    unsigned int length() const { return len_; }
    const char  *c_str()  const { return buf_ ? buf_ : ""; }
    char        *begin()        { return buf_; }
    char        *end()          { return buf_ + len_; }

    bool reserve(unsigned int size);
    void clear() { len_ = 0; if (buf_) buf_[0] = '\0'; }
    bool isEmpty() const { return len_ == 0; }

    bool concat(const String &s);
    bool concat(const char *s);
    bool concat(char c);
    bool concat(long v);
    bool concat(unsigned long v);
    bool concat(int v)          { return concat(static_cast<long>(v)); }
    bool concat(unsigned int v) { return concat(static_cast<unsigned long>(v)); }
    bool concat(double v);

    template <typename T> String &operator+=(T rhs) { concat(rhs); return *this; }
    String &operator+=(const char *rhs)   { concat(rhs); return *this; }
    String &operator+=(const String &rhs) { concat(rhs); return *this; }

    int compareTo(const String &s) const;
    bool equals(const String &s) const;
    bool equals(const char *s) const;
    bool equalsIgnoreCase(const String &s) const;

    bool operator==(const String &rhs) const { return equals(rhs); }
    bool operator==(const char *rhs)   const { return equals(rhs); }
    bool operator!=(const String &rhs) const { return !equals(rhs); }
    bool operator!=(const char *rhs)   const { return !equals(rhs); }
    bool operator< (const String &rhs) const { return compareTo(rhs) <  0; }
    bool operator> (const String &rhs) const { return compareTo(rhs) >  0; }
    bool operator<=(const String &rhs) const { return compareTo(rhs) <= 0; }
    bool operator>=(const String &rhs) const { return compareTo(rhs) >= 0; }

    char operator[](unsigned int index) const;
    char charAt(unsigned int index) const { return operator[](index); }
    void setCharAt(unsigned int index, char c);

    bool startsWith(const String &prefix) const;
    bool endsWith(const String &suffix) const;

    int indexOf(char c, unsigned int from = 0) const;
    int indexOf(const String &s, unsigned int from = 0) const;
    int lastIndexOf(char c) const;

    String substring(unsigned int from) const { return substring(from, len_); }
    String substring(unsigned int from, unsigned int to) const;

    void replace(char find, char replace);
    void remove(unsigned int index) { remove(index, len_); }
    void remove(unsigned int index, unsigned int count);
    void toLowerCase();
    void toUpperCase();
    void trim();

    long   toInt()    const;
    float  toFloat()  const;
    double toDouble() const;

private:
    char        *buf_;
    unsigned int cap_;
    unsigned int len_;

    void init();
    void invalidate();
    bool changeBuffer(unsigned int maxlen);
    bool copy(const char *s, unsigned int len);
    String &fromNumber(unsigned long v, unsigned char base, bool neg);
};

inline String operator+(const String &lhs, const String &rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, const char *rhs)   { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, char rhs)          { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, int rhs)           { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, long rhs)          { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, unsigned long rhs) { String r(lhs); r += rhs; return r; }
inline String operator+(const String &lhs, double rhs)        { String r(lhs); r += rhs; return r; }
inline String operator+(const char *lhs, const String &rhs)   { String r(lhs); r += rhs; return r; }

#endif /* SL6806_WSTRING_H */
