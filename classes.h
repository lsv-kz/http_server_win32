#ifndef CLASSES_H_
#define CLASSES_H_

#include "main.h"

//======================================================================
struct Range {
    long long start;
    long long end;
    long long len;
};
//----------------------------------------------------------------------
class ArrayRanges // except
{
protected:
    const int ADDITION = 8;
    Range* range;
    unsigned int sizeBuf;
    unsigned int lenBuf;
    int numPart;
    long long sizeFile;

    int check_ranges();
    int parse_ranges(char* sRange, String& ss);

public:
    ArrayRanges(const ArrayRanges&) = delete;
    ArrayRanges()
    {
        sizeBuf = lenBuf = numPart = 0;
        sizeFile = 0LL;
        range = NULL;
    }

    ~ArrayRanges()
    {
        if (range) delete[] range;
    }

    int resize(unsigned int n)
    {
        if (n <= lenBuf)
            return 1;
        Range * tmp = new(std::nothrow) Range[n];
        if (!tmp)
            return 1;
        for (unsigned int c = 0; c < lenBuf; ++c)
            tmp[c] = range[c];
        if (range)
            delete[] range;
        range = tmp;
        sizeBuf = n;
        return 0;
    }

    ArrayRanges & operator << (const Range & val)
    {
        if (lenBuf >= sizeBuf)
            if (resize(sizeBuf + ADDITION)) throw ENOMEM;
        range[lenBuf++] = val;
        return *this;
    }

    Range * get(unsigned int i)
    {
        if (i < lenBuf)
            return range + i;
        else
            return NULL;
    }

    int len() { return lenBuf; }
    int size() { return sizeBuf; }

    int create_ranges(char* s, long long sz);
};

#endif
