#include "range.h"

//======================================================================
int Ranges::check_ranges()
{
    int numPart, maxIndex, n;
    Range* r = range;
    numPart = lenBuf;
    maxIndex = n = numPart - 1;

    while (n > 0)
    {
        for (int i = n - 1; i >= 0; )
        {
            if (((r[n].end + 1) >= r[i].start) && ((r[i].end + 1) >= r[n].start))
            {
                if (r[n].start < r[i].start)
                    r[i].start = r[n].start;

                if (r[n].end > r[i].end)
                    r[i].end = r[n].end;

                r[i].len = r[i].end - r[i].start + 1;
                r[n].len = 0;

                if ((maxIndex > n) && (r[maxIndex].len > 0))
                {
                    r[n].start = r[maxIndex].start;
                    r[n].end = r[maxIndex].end;
                    r[n].len = r[maxIndex].len;
                    r[maxIndex].len = 0;
                    maxIndex--;
                    n = maxIndex - 1;
                }
                else
                {
                    maxIndex--;
                    n = maxIndex;
                }
                numPart--;
                i = n - 1;
            }
            else
                i--;
        }
        n--;
    }

    return numPart;
}
//======================================================================
int Ranges::parse_ranges(char* sRange, String& ss)
{
    char* p0 = sRange, * p;
    long long size = sizeFile;
    int numPart = 0;

    for (; *p0; p0++)
    {
        if ((*p0 != ' ') && (*p0 != '\t'))
            break;
    }

    for (p = p0; *p; )
    {
        long long start = 0, end = 0;
        char ch = *p;
        if ((ch >= '0') && (ch <= '9'))
        {
            start = strtoll(p, &p, 10);
            if (*p == '-')
            {
                ch = *(++p);
                if ((ch >= '0') && (ch <= '9'))// [10-50]
                {
                    end = strtoll(p, &p, 10);
                    if ((*p != ',') && (*p != 0))
                        break;
                }
                else if ((ch == ',') || (ch == 0))// [10-]
                    end = size - 1;
                else
                    break;
            }
            else
            {
                return 0;
            }
        }
        else if (ch == '-')
        {
            if ((*(p + 1) >= '0') && (*(p + 1) <= '9'))// [-50]
            {
                end = strtoll(p, &p, 10);
                if ((*p != ',') && (*p != 0))
                    break;
                start = size + end;
                end = size - 1;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            break;
        }

        if (end >= size)
            end = size - 1;

        if ((start < size) && (end >= start) && (start >= 0))
        {
            ss << start << "-" << end;
            if (*p == ',')
                ss << ",";
            numPart++;
            if (*p == 0)
                break;
        }
        p++;
    }

    return numPart;
}
//======================================================================
int Ranges::create_ranges(char* s, long long sz)
{
    String ss(128);
    if (ss.error())
    {
        return 0;
    }
    sizeFile = sz;
    numPart = parse_ranges(s, ss);
    if (numPart > 0)
    {
        if (resize(numPart))
        {
            numPart = 0;
            return 0;
        }

        const char* p = ss.c_str();
        char* pp;
        for (int i = 0; i < numPart; ++i)
        {
            long long start, end;
            start = strtoll(p, &pp, 10);
            pp++;
            p = pp;

            end = strtoll(p, &pp, 10);
            pp++;
            p = pp;

            (*this) << Range{ start, end, end - start + 1 };
        }

        if (numPart > 1)
            numPart = check_ranges();
    }
    return numPart;
}
