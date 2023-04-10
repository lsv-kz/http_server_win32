#include "main.h"

using namespace std;

//======================================================================
int PrintError(const char* f, int line, const char* s)
{
    LPVOID lpMsgBuf = NULL;
    DWORD err = GetLastError();

    FormatMessage
    (
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), // 1. LANG_NEUTRAL  
        (LPTSTR)& lpMsgBuf,
        0,
        NULL
    );

    if (lpMsgBuf)
    {
        print_err("<%s:%d> %s: (%ld)%s", f, line, s, err, (char*)lpMsgBuf);
        LocalFree(lpMsgBuf);
    }
    return err;
}
//======================================================================
int ErrorStrSock(const char* f, int line, const char* s)
{
    LPVOID lpMsgBuf = NULL;
    int err = WSAGetLastError();

    FormatMessage
    (
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), // 1. LANG_NEUTRAL  
        (LPTSTR)& lpMsgBuf,
        0,
        NULL
    );

    if (lpMsgBuf)
    {
        print_err("<%s:%d> %s: (%ld)%s", f, line, s, err, (char*)lpMsgBuf);
        LocalFree(lpMsgBuf);
    }
    return err;
}
//======================================================================
string get_time()
{
    __time64_t now = 0;
    struct tm t;
    char s[40];

    _time64(&now);
    _gmtime64_s(&t, &now);
 
    strftime(s, sizeof(s), "%a, %d %b %Y %H:%M:%S GMT", &t);
    return s;
}
//======================================================================
void get_time(string& str)
{
    __time64_t now = 0;
    struct tm t;
    char s[40];

    _time64(&now);
    _gmtime64_s(&t, &now);
 
    strftime(s, sizeof(s), "%a, %d %b %Y %H:%M:%S GMT", &t);
    str = s;
}
//======================================================================
string log_time()
{
    __time64_t now = 0;
    struct tm t;
    char s[40];

    _time64(&now);
    _gmtime64_s(&t, &now);

    strftime(s, sizeof(s), "%d/%b/%Y:%H:%M:%S GMT", &t);
    return s;
}
//======================================================================
const char* strstr_case(const char* s1, const char* s2)
{
    char c1, c2;
    const char *p1, *p2;

    if (!s1 || !s2) return NULL;
    if (*s2 == 0) return s1;

    int diff = ('a' - 'A');
    
    for (; ; ++s1)
    {
        c1 = *s1;
        if (!c1) break;
        c2 = *s2;
        c1 += (c1 >= 'A') && (c1 <= 'Z') ? diff : 0;
        c2 += (c2 >= 'A') && (c2 <= 'Z') ? diff : 0;
        if (c1 == c2)
        {
            p1 = s1;
            p2 = s2;
            ++s1;
            ++p2;

            for (; ; ++s1, ++p2)
            {
                c2 = *p2;
                if (!c2) return p1;

                c1 = *s1;
                if (!c1) return NULL;

                c1 += (c1 >= 'A') && (c1 <= 'Z') ? diff : 0;
                c2 += (c2 >= 'A') && (c2 <= 'Z') ? diff : 0;
                if (c1 != c2)
                    break;
            }
        }
    }

    return NULL;
}
//======================================================================
int strlcmp_case(const char* s1, const char* s2, int len)
{
    char c1, c2;

    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;

    for (; len > 0; --len, ++s1, ++s2)
    {
        c1 = *s1;
        c2 = *s2;
        if (!c1 && !c2) return 0;
        if (!c1) return -1;
        if (!c2) return 1;

        c1 += (c1 >= 'A') && (c1 <= 'Z') ? ('a' - 'A') : 0;
        c2 += (c2 >= 'A') && (c2 <= 'Z') ? ('a' - 'A') : 0;

        if (c1 > c2) return 1;
        if (c1 < c2) return -1;
    }

    return 0;
}
//======================================================================
int strcmp_case(const char* s1, const char* s2)
{
    char c1, c2;

    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;

    for (; ; ++s1, ++s2)
    {
        c1 = *s1;
        c2 = *s2;
        if (!c1 && !c2) return 0;
        if (!c1) return -1;
        if (!c2) return 1;

        c1 += (c1 >= 'A') && (c1 <= 'Z') ? ('a' - 'A') : 0;
        c2 += (c2 >= 'A') && (c2 <= 'Z') ? ('a' - 'A') : 0;

        if (c1 > c2) return 1;
        if (c1 < c2) return -1;
    }

    return 0;
}
//======================================================================
int get_int_method(char* s)
{
    if (!memcmp(s, "GET", 3))
        return M_GET;
    else if (!memcmp(s, "POST", 4))
        return M_POST;
    else if (!memcmp(s, "HEAD", 4))
        return M_HEAD;
    else if (!memcmp(s, "OPTIONS", 7))
        return M_OPTIONS;
    else if (!memcmp(s, "CONNECT", 7))
        return M_CONNECT;
    else
        return 0;
}
//======================================================================
const char* get_str_method(int i)
{
    if (i == M_GET)
        return "GET";
    else if (i == M_POST)
        return "POST";
    else if (i == M_HEAD)
        return "HEAD";
    else if (i == M_OPTIONS)
        return "OPTIONS";
    else if (i == M_CONNECT)
        return "CONNECT";
    return "";
}
//======================================================================
int get_int_http_prot(char* s)
{
    if (!memcmp(s, "HTTP/1.1", 8))
        return HTTP11;
    else if (!memcmp(s, "HTTP/1.0", 8))
        return HTTP10;
    else if (!memcmp(s, "HTTP/0.9", 8))
        return HTTP09;
    else if (!memcmp(s, "HTTP/2", 6))
        return HTTP2;
    else
        return 0;
}
//======================================================================
const char* get_str_http_prot(int i)
{
    if (i == HTTP11)
        return "HTTP/1.1";
    else if (i == HTTP10)
        return "HTTP/1.0";
    else if (i == HTTP09)
        return "HTTP/0.9";
    else if (i == HTTP2)
        return "HTTP/2";
    return "";
}
//======================================================================
const char* strstr_lowercase(const char* s1, const char* s2)
{
    int i, len = (int)strlen(s2);
    const char* p = s1;
    for (i = 0; *p; ++p)
    {
        if (tolower(*p) == tolower(s2[0]))
        {
            for (i = 1; ; i++)
            {
                if (i == len)
                    return p;
                if (tolower(p[i]) != tolower(s2[i]))
                    break;
            }
        }
    }
    return NULL;
}
//======================================================================
const char* content_type(const wchar_t* path)
{
    string s;
    if (utf16_to_utf8(path, s))
    {
        print_err("<%s:%d> Error utf16_to_mbs()\n", __func__, __LINE__);
        return "";
    }

    const char* p = strrchr(s.c_str(), '.');
    if (!p)
    {
        return "";
    }
    //       video
    if (!strlcmp_case(p, ".ogv", 4)) return "video/ogg";
    else if (!strlcmp_case(p, ".mp4", 4)) return "video/mp4";
    else if (!strlcmp_case(p, ".avi", 4)) return "video/x-msvideo";
    else if (!strlcmp_case(p, ".mov", 4)) return "video/quicktime";
    else if (!strlcmp_case(p, ".mkv", 4)) return "video/x-matroska";
    else if (!strlcmp_case(p, ".flv", 4)) return "video/x-flv";
    else if (!strlcmp_case(p, ".mpeg", 5) || !strlcmp_case(p, ".mpg", 4)) return "video/mpeg";
    else if (!strlcmp_case(p, ".asf", 4)) return "video/x-ms-asf";
    else if (!strlcmp_case(p, ".wmv", 4)) return "video/x-ms-wmv";
    else if (!strlcmp_case(p, ".swf", 4)) return "application/x-shockwave-flash";
    else if (!strlcmp_case(p, ".3gp", 4)) return "video/video/3gpp";

    //       sound
    else if (!strlcmp_case(p, ".mp3", 4)) return "audio/mpeg";
    else if (!strlcmp_case(p, ".wav", 4)) return "audio/x-wav";
    else if (!strlcmp_case(p, ".ogg", 4)) return "audio/ogg";
    else if (!strlcmp_case(p, ".pls", 4)) return "audio/x-scpls";
    else if (!strlcmp_case(p, ".aac", 4)) return "audio/aac";
    else if (!strlcmp_case(p, ".aif", 4)) return "audio/x-aiff";
    else if (!strlcmp_case(p, ".ac3", 4)) return "audio/ac3";
    else if (!strlcmp_case(p, ".voc", 4)) return "audio/x-voc";
    else if (!strlcmp_case(p, ".flac", 5)) return "audio/flac";
    else if (!strlcmp_case(p, ".amr", 4)) return "audio/amr";
    else if (!strlcmp_case(p, ".au", 3)) return "audio/basic";

    //       image
    else if (!strlcmp_case(p, ".gif", 4)) return "image/gif";
    else if (!strlcmp_case(p, ".svg", 4) || !strlcmp_case(p, ".svgz", 5)) return "image/svg+xml";
    else if (!strlcmp_case(p, ".png", 4)) return "image/png";
    else if (!strlcmp_case(p, ".ico", 4)) return "image/vnd.microsoft.icon";
    else if (!strlcmp_case(p, ".jpeg", 5) || !strlcmp_case(p, ".jpg", 4)) return "image/jpeg";
    else if (!strlcmp_case(p, ".djvu", 5) || !strlcmp_case(p, ".djv", 4)) return "image/vnd.djvu";
    else if (!strlcmp_case(p, ".tiff", 5)) return "image/tiff";
    //       text
    else if (!strlcmp_case(p, ".txt", 4)) return "text/plain; charset=utf-8"; // return istextfile(s);
    else if (!strlcmp_case(p, ".html", 5) || !strlcmp_case(p, ".htm", 4) || !strlcmp_case(p, ".shtml", 6)) return "text/html; charset=utf-8"; // cp1251
    else if (!strlcmp_case(p, ".css", 4)) return "text/css";

    //       application
    else if (!strlcmp_case(p, ".pdf", 4)) return "application/pdf";
    else if (!strlcmp_case(p, ".gz", 3)) return "application/gzip";

    return "";
}
//======================================================================
int clean_path(char* path)
{
    int i = 0, o = 0;
    char ch;

    while ((ch = *(path + o)))
    {
        if (!memcmp(path + o, "/../", 4))
        {
            if (i != 0)
            {
                for (--i; i > 0; --i)
                {
                    if (*(path + i) == '/')
                        break;
                }
            }
            o += 3;
        }
        else if (!memcmp(path + o, "//", 2))
            o += 1;
        else if (!memcmp(path + o, "/./", 3))
            o += 2;
        else
        {
            if (o != i)
                * (path + i) = ch;
            ++i;
            ++o;
        }
    }

    *(path + i) = 0;

    return i;
}
//======================================================================
int parse_startline_request(Connect* req, char* s, int len)
{
    char* p, tmp[16];
    //----------------------------- method -----------------------------
    p = tmp;
    int i = 0, n = 0;
    while ((i < len) && (n < (int)sizeof(tmp)))
    {
        char ch = s[i++];
        if ((ch != '\x20') && (ch != '\r') && (ch != '\n'))
            p[n++] = ch;
        else
            break;
    }
    p[n] = 0;
    req->reqMethod = get_int_method(tmp);
    if (!req->reqMethod) return -RS400;
    //------------------------------- uri ------------------------------
    char ch = s[i];
    if ((ch == '\x20') || (ch == '\r') || (ch == '\n'))
    {
        return -RS400;
    }

    req->uri = s + i;
    while (i < len)
    {
        char ch = s[i];
        if ((ch == '\x20') || (ch == '\r') || (ch == '\n') || (ch == '\0'))
            break;
        ++i;
    }

    if (s[i] == '\r')// HTTP/0.9
    {
        req->httpProt = HTTP09;
        s[i] = 0;
        return 0;
    }

    if (s[i] == '\x20')
        s[i++] = 0;
    else
        return -RS400;
    //------------------------------ version ---------------------------
    ch = s[i];
    if ((ch == '\x20') || (ch == '\r') || (ch == '\n'))
        return -RS400;

    p = tmp;
    n = 0;
    while ((i < len) && (n < (int)sizeof(tmp)))
    {
        char ch = s[i++];
        if ((ch != '\x20') && (ch != '\r') && (ch != '\n'))
            p[n++] = ch;
        else
            break;
    }
    p[n] = 0;

    if (!(req->httpProt = get_int_http_prot(tmp)))
    {
        print_err(req, "<%s:%d> Error version protocol\n", __func__, __LINE__);
        req->httpProt = HTTP11;
        return -RS400;
    }

    return 0;
}
//======================================================================
int parse_headers(Connect* req, char* s, int len)
{
    char* pName = s, * pVal, * p;
    if (!s)
    {
        print_err(req, "<%s:%d> Error: header is empty\n", __func__, __LINE__);
        return -1;
    }

    char ch;
    p = pName;
    int colon = 0;
    while ((ch = *p))
    {
        if (ch == ':')
            colon = 1;
        else if ((ch == ' ') || (ch == '\t') || (ch == '\n') || (ch == '\r'))
        {
            if (colon == 0)
                return -RS400;
            *(p++) = 0;
            break;
        }
        else
            *p = tolower(ch);
        p++;
    }

    if (*p == ' ')
        return -RS400;
    pVal = p;

    if (!strlcmp_case(pName, "connection:", 11))
    {
        req->req_hdrs.iConnection = req->req_hdrs.countReqHeaders;
        if (strstr_case(pVal, "keep-alive"))
            req->connKeepAlive = 1;
        else
            req->connKeepAlive = 0;
    }
    else if (!strlcmp_case(pName, "host:", 5))
    {
        req->req_hdrs.iHost = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "range:", 6))
    {
        char* p = strchr(pVal, '=');
        if (p)
            req->sRange = p + 1;
        else
            req->sRange = NULL;
        req->req_hdrs.iRange = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "If-Range:", 9))
    {
        req->req_hdrs.iIf_Range = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "referer:", 8))
    {
        req->req_hdrs.iReferer = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "user-agent:", 11))
    {
        req->req_hdrs.iUserAgent = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "upgrade:", 8))
    {
        req->req_hdrs.iUpgrade = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "content-length:", 15))
    {
        req->req_hdrs.reqContentLength = atoll(pVal);
        req->req_hdrs.iReqContentLength = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "content-type:", 13))
    {
        req->req_hdrs.iReqContentType = req->req_hdrs.countReqHeaders;
    }
    else if (!strlcmp_case(pName, "accept-encoding:", 16))
    {
        req->req_hdrs.iAcceptEncoding = req->req_hdrs.countReqHeaders;
    }

    req->req_hdrs.Name[req->req_hdrs.countReqHeaders] = pName;
    req->req_hdrs.Value[req->req_hdrs.countReqHeaders] = pVal;
    ++req->req_hdrs.countReqHeaders;

    return 0;
}
//======================================================================
void path_correct(wstring & path)
{
    size_t len = path.size(), i = 0;
    while (i < len)
    {
        if (path[i] == '\\')
            path[i] = '/';
        ++i;
    }
}
//======================================================================
const char *get_str_operation(OPERATION_TYPE n)
{
    switch (n)
    {
        case READ_REQUEST:
            return "READ_REQUEST";
        case SEND_RESP_HEADERS:
            return "SEND_RESP_HEADERS";
        case SEND_ENTITY:
            return "SEND_ENTITY";
        case DYN_PAGE:
            return "DYN_PAGE";
    }

    return "?";
}
//======================================================================
const char *get_cgi_status(CGI_STATUS n)
{
    switch (n)
    {
        case CGI_CREATE_PROC:
            return "CGI_CREATE_PROC";
        case CGI_STDIN:
            return "CGI_STDIN";
        case CGI_READ_HTTP_HEADERS:
            return "CGI_READ_HTTP_HEADERS";
        case CGI_SEND_HTTP_HEADERS:
            return "CGI_SEND_HTTP_HEADERS";
        case CGI_SEND_ENTITY:
            return "CGI_SEND_ENTITY";
    }

    return "?";
}
//======================================================================
const char *get_fcgi_status(FCGI_STATUS n)
{
    switch (n)
    {
        case FASTCGI_CONNECT:
            return "FASTCGI_CONNECT";
        case FASTCGI_BEGIN:
            return "FASTCGI_BEGIN";
        case FASTCGI_PARAMS:
            return "FASTCGI_PARAMS";
        case FASTCGI_STDIN:
            return "FASTCGI_STDIN";
        case FASTCGI_READ_HEADER:
            return "FASTCGI_READ_HEADER";
        case FASTCGI_READ_HTTP_HEADERS:
            return "FASTCGI_READ_HTTP_HEADERS";
        case FASTCGI_SEND_HTTP_HEADERS:
            return "FASTCGI_SEND_HTTP_HEADERS";
        case FASTCGI_SEND_ENTITY:
            return "FASTCGI_SEND_ENTITY";
        case FASTCGI_READ_ERROR:
            return "FASTCGI_READ_ERROR";
        case FASTCGI_READ_PADDING:
            return "FASTCGI_READ_PADDING";
        case FASTCGI_CLOSE:
            return "FASTCGI_CLOSE";
    }

    return "?";
}
//======================================================================
const char *get_scgi_status(SCGI_STATUS n)
{
    switch (n)
    {
        case SCGI_CONNECT:
            return "SCGI_CONNECT";
        case SCGI_PARAMS:
            return "SCGI_PARAMS";
        case SCGI_STDIN:
            return "SCGI_STDIN";
        case SCGI_READ_HTTP_HEADERS:
            return "SCGI_READ_HTTP_HEADERS";
        case SCGI_SEND_HTTP_HEADERS:
            return "SCGI_SEND_HTTP_HEADERS";
        case SCGI_SEND_ENTITY:
            return "SCGI_SEND_ENTITY";
    }

    return "?";
}
//======================================================================
const char *get_cgi_type(CGI_TYPE n)
{
    switch (n)
    {
        case CGI_TYPE_NONE:
            return "CGI_TYPE_NONE";
        case CGI:
            return "CGI";
        case PHPCGI:
            return "PHPCGI";
        case PHPFPM:
            return "PHPFPM";
        case FASTCGI:
            return "FASTCGI";
        case SCGI:
            return "SCGI";
    }

    return "?";
}
//======================================================================
const char *get_cgi_dir(DIRECT n)
{
    switch (n)
    {
        case FROM_CGI:
            return "FROM_CGI";
        case TO_CGI:
            return "TO_CGI";
        case FROM_CLIENT:
            return "FROM_CLIENT";
        case TO_CLIENT:
            return "TO_CLIENT";
    }

    return "?";
}
