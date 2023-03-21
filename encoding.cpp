#include "main.h"

using namespace std;

//======================================================================
int utf16_to_utf8(const wstring& ws, string& s)
{
    size_t wlen = ws.size(), i = 0;
    s.clear();

    while (i < wlen)
    {
        wchar_t wc = ws[i];
        if (wc <= 0x7f)
        {
            s += (char)wc;
        }
        else if (wc <= 0x7ff)
        {
            s += (0xc0 | (char)(wc >> 6));
            s += (0x80 | ((char)wc & 0x3f));
        }
        else if (wc <= 0xffff)
        {
            s += (0xE0 | (wc >> 12));
            s += (0x80 | ((wc >> 6) & 0x3F));
            s += (0x80 | ((wc >> 0) & 0x3F));
        }
        else
        {
            print_err("<%s:%d> utf-32\n", __func__, __LINE__);
            return 1;
        }
        ++i;
    }

    return 0;
}
//======================================================================
int utf16_to_utf8(const wstring& ws, String& s)
{
    size_t wlen = ws.size(), i = 0;
    s.clear();

    while (i < wlen)
    {
        wchar_t wc = ws[i];
        if (wc <= 0x7f)
        {
            s << (char)wc;
        }
        else if (wc <= 0x7ff)
        {
            s << (char)(0xc0 | (char)(wc >> 6));
            s << (char)(0x80 | ((char)wc & 0x3f));
        }
        else if (wc <= 0xffff)
        {
            s << (char)(0xE0 | (wc >> 12));
            s << (char)(0x80 | ((wc >> 6) & 0x3F));
            s << (char)(0x80 | ((wc >> 0) & 0x3F));
        }
        else
        {
            print_err("<%s:%d> utf-32\n", __func__, __LINE__);
            return 1;
        }
        ++i;
    }

    return 0;
}
//======================================================================
/*int utf16_to_utf8(const wchar_t* ws, string & s)
{
    size_t wlen = wcslen(ws), i = 0;
    s.clear();

    while (i < wlen)
    {
        wchar_t wc = ws[i];
        if (wc <= 0x7f)
        {
            s += (char)wc;
        }
        else if (wc <= 0x7ff)
        {
            s += (0xc0 | (char)(wc >> 6));
            s += (0x80 | ((char)wc & 0x3f));
        }
        else if (wc <= 0xffff)
        {
            s += (0xE0 | (wc >> 12));
            s += (0x80 | ((wc >> 6) & 0x3F));
            s += (0x80 | ((wc >> 0) & 0x3F));
        }
        else
        {
            printf("<%s:%d> utf-32\n", __func__, __LINE__);
            return 1;
        }
        ++i;
    }

    return 0;
}*/
//======================================================================
int utf8_to_utf16(const char* u8, wstring& ws)
{
    size_t len = strlen(u8), i = 0;
    size_t num;
    ws.clear();

    while (i < len)
    {
        unsigned char ch = u8[i++];
        wchar_t uni;

        if (ch <= 0x7F)
        {
            uni = ch;
            num = 0;
        }
        else if (ch <= 0xBF)
        {
            ws = L"not a UTF-8 string\n";
            return 1;
        }
        else if (ch <= 0xDF)
        {
            uni = ch & 0x1F;
            num = 1;
        }
        else if (ch <= 0xEF)
        {
            uni = ch & 0x0F;
            num = 2;
        }
        else if (ch <= 0xF7)
        {
            uni = ch & 0x07;
            num = 3;
        }
        else
        {
            ws = L"not a UTF-8 string\n";
            return 2;
        }

        for (size_t j = 0; j < num; ++j)
        {
            if (i == len)
            {
                ws = L"not a UTF-8 string\n";
                return 3;
            }

            unsigned char ch = u8[i++];
            if (ch < 0x80 || ch > 0xBF)
            {
                ws = L"not a UTF-8 string\n";
                return 4;
            }

            uni <<= 6;
            uni += ch & 0x3F;
        }
        if (uni >= 0xD800 && uni <= 0xDFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 5;
        }

        if (uni > 0x10FFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 6;
        }

        ws += uni;
    }

    return 0;
}
//======================================================================
int utf8_to_utf16(const string & u8, wstring & ws)
{
    size_t len = u8.size(), i = 0;
    size_t num;
    ws.clear();

    while (i < len)
    {
        unsigned char ch = u8[i++];
        wchar_t uni;

        if (ch <= 0x7F)
        {
            uni = ch;
            num = 0;
        }
        else if (ch <= 0xBF)
        {
            ws = L"not a UTF-8 string\n";
            return 1;
        }
        else if (ch <= 0xDF)
        {
            uni = ch & 0x1F;
            num = 1;
        }
        else if (ch <= 0xEF)
        {
            uni = ch & 0x0F;
            num = 2;
        }
        else if (ch <= 0xF7)
        {
            uni = ch & 0x07;
            num = 3;
        }
        else
        {
            ws = L"not a UTF-8 string\n";
            return 2;
        }

        for (size_t j = 0; j < num; ++j)
        {
            if (i == len)
            {
                ws = L"not a UTF-8 string\n";
                return 3;
            }

            unsigned char ch = u8[i++];
            if (ch < 0x80 || ch > 0xBF)
            {
                ws = L"not a UTF-8 string\n";
                return 4;
            }

            uni <<= 6;
            uni += ch & 0x3F;
        }

        if (uni >= 0xD800 && uni <= 0xDFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 5;
        }

        if (uni > 0x10FFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 6;
        }

        ws += uni;
    }

    return 0;
}
//======================================================================
int utf8_to_utf16(const String& u8, wstring& ws)
{
    size_t len = u8.str().size(), i = 0;
    size_t num;
    ws.clear();

    while (i < len)
    {
        unsigned char ch = u8[i++];
        wchar_t uni;

        if (ch <= 0x7F)
        {
            uni = ch;
            num = 0;
        }
        else if (ch <= 0xBF)
        {
            ws = L"not a UTF-8 string\n";
            return 1;
        }
        else if (ch <= 0xDF)
        {
            uni = ch & 0x1F;
            num = 1;
        }
        else if (ch <= 0xEF)
        {
            uni = ch & 0x0F;
            num = 2;
        }
        else if (ch <= 0xF7)
        {
            uni = ch & 0x07;
            num = 3;
        }
        else
        {
            ws = L"not a UTF-8 string\n";
            return 2;
        }

        for (size_t j = 0; j < num; ++j)
        {
            if (i == len)
            {
                ws = L"not a UTF-8 string\n";
                return 3;
            }

            unsigned char ch = u8[i++];
            if (ch < 0x80 || ch > 0xBF)
            {
                ws = L"not a UTF-8 string\n";
                return 4;
            }

            uni <<= 6;
            uni += ch & 0x3F;
        }

        if (uni >= 0xD800 && uni <= 0xDFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 5;
        }

        if (uni > 0x10FFFF)
        {
            ws = L"not a UTF-8 string\n";
            return 6;
        }

        ws += uni;
    }

    return 0;
}
//======================================================================
string encode(const string& s_in)
{
    unsigned char c, d;
    int len_in = s_in.size(), i = 0;
    char Az09[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz" "0123456789" "/:-_.!~*'()";
    string s_out;
    while (len_in > 0)
    {
        c = s_in[i++];
        --len_in;
        if (c < 0x7f)
        {
            if (!strchr(Az09, c))
            {
                s_out += '%';
                d = c >> 4;
                s_out += (d < 10 ? d + '0' : d + '7');
                d = c & 0x0f;
                s_out += (d < 10 ? d + '0' : d + '7');
            }
            else if (c == ' ')
                s_out += '+';
            else
                s_out += c;
        }
        else
        {
            s_out += '%';
            d = c >> 4;
            s_out += (d < 10 ? d + '0' : d + '7');
            d = c & 0x0f;
            s_out += (d < 10 ? d + '0' : d + '7');
        }
    }

    return s_out;
}
//======================================================================
int decode(const char* s_in, size_t len_in, char* s_out, int len)
{
    if (!s_in || !s_out)
        return -1;
    char tmp[3];
    char* p = s_out;
    unsigned char c;
    long cnt = 0, i;

    while (len_in > 0)
    {
        c = *(s_in++);
        if (c == '%')
        {
            if (len_in < 3)
            {
                *p = 0;
                return 0;
            }

            tmp[0] = *(s_in++);
            tmp[1] = *(s_in++);
            tmp[2] = 0;
            len_in -= 2;

            const char* pp = tmp;
            i = strtol((char*)pp, (char**)&pp, 16);
            if (*pp != 0)
            {
                *p = 0;
                return 0;
            }

            *p = (char)i;
        }
        else if (c == '+')
            *p = ' ';
        else if (c == '\\')
        {
            *p = 0;
            return 0;
        }
        else
            *p = c;

        --len_in;
        --len;
        ++cnt;
        if (len <= 0)
        {
            *p = 0;
            return 0;
        }
        ++p;
    }

    *p = 0;
    return cnt;
}
