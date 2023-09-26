#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <locale.h>
#include <wchar.h>

char s_time[128];

//======================================================================
int utf8_to_utf16(wchar_t *ws, int wsize, const char *u8)
{
    int len = strlen(u8), i = 0, wi = 0;
    size_t num;
    ws[0] = L'\0';
    
    while (i < len)
    {
        unsigned char ch = u8[i++];
        wchar_t wch;
        
        if (ch <= 0x7F)
        {
            wch = ch;
            num = 0;
        }
        else if (ch <= 0xBF)
        {
            ws = L"not a UTF-8 string\n";
            return 1;
        }
        else if (ch <= 0xDF)
        {
            wch = ch & 0x1F;
            num = 1;
        }
        else if (ch <= 0xEF)
        {
            wch = ch & 0x0F;
            num = 2;
        }
        else if (ch <= 0xF7)
        {
            wch = ch & 0x07;
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
                return 3;
            }
            
            unsigned char ch = u8[i++];
            if (ch < 0x80 || ch > 0xBF)
            {
                return 4;
            }
                
            wch <<= 6;
            wch += ch & 0x3F;
        }
        
        if (wch >= 0xD800 && wch <= 0xDFFF)
        {
            return 5;
        }
        
        if (wch > 0x10FFFF)
        {
            return 6;
        }
        
        if (wi < (wsize-1))
        {
            ws[wi] = wch;
            ++wi;
        }
        else
        {
            ws[wi] = L'\0';
            return 7;
        }
    }
    ws[wi] = L'\0';
    return 0;
}
/*====================================================================*/
void head_html(void)
{
    printf( "<!DOCTYPE html>\n"
            "<html>\n"
            " <head>\n"
            "  <meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\">\n"
            "  <title>File downloaded</title>\n"
            " </head>\n"
            " <body>\n");
}
/*====================================================================*/
int str_cat(char *dst, char *src, int size_dst)
{
    if (!dst || !src)
        return -1;
    
    int len_src = strlen(src);
    int len_dst = strlen(dst);
    int i;
    
    if ((size_dst - len_dst) <= 0)
        return -1;

    if ((size_dst - len_dst) < (len_src + 1))
        return -1;

    for (i = 0; i < len_src; i++)
    {
        dst[i + len_dst] = src[i];
    }

    dst[i + len_dst] = '\0';
    return i;
}
/*====================================================================*/
int find_in_str(char *s_in, char *s_out, int size_out, char *s1, char *s2)
{
    if (!s_in || !s_out || !s1 || !s2)
        return 0;
    
    char *p1, *p2;
    int len = strlen(s1), n;
    *s_out = 0;
    if ((p1 = strstr(s_in, s1)))
    {
        p1 += len;
        if (*s2 == 0)
            n = strlen(p1);
        else
        {
            p2 = strstr(p1, s2);
            if (!p2)
                return 0;
            n = p2 - p1;
        }
        
        if(n < size_out)
        {
            memcpy(s_out, p1, n);
            s_out[n] = 0;
            return n;
        }
    }
    return 0;
}
/*====================================================================*/
int read_line(char *buf, int size_buf, int *content_len)
{
    int n;
    
    buf[0] = '\0';
    if(!fgets (buf, size_buf, stdin))
    {
        if(!strlen(buf))
            return 0;
    }
    
    n = strlen(buf);
    *content_len -= n;
    return n;
}
/*====================================================================*/
int read_head(char *buf, int buf_size, int *content_len)
{
    int n = 0, len;
    char s[1024], *p = buf;
    
    *buf = 0; 
    while(1)
    {
        if(!fgets (s, sizeof(s), stdin))
            return -1;

        len = strlen(s);
        if (buf_size <= len)
            return -1;
        
        
        n += len;
        *content_len -= len;
        
        memcpy(p, s, len);
        p += len;
        *p = 0;
        if(!strcspn(s, "\r\n"))
            break;
        buf_size -= len;
    }
    return n;
}
/*====================================================================*/
int write_to_file(FILE *f, int len, int *content_len)
{
    int n, m;
    long long wr_bytes;
    char buf[2048];

    for(n = 0, wr_bytes = 0; len > 0; )
    {
        if(len > sizeof(buf))
            //------------- read from stdin -------------
            n = fread(buf, 1, sizeof(buf), stdin);
        else
            //------------- read from stdin -------------
            n = fread(buf, 1, len, stdin);

        if(n <= 0)
            return n;
        len -= n;
        buf[n]=0;
        *content_len -= n;
        m = fwrite(buf, 1, n, f);
        if(m == -1)
            return -2;
        wr_bytes = wr_bytes + m;
        if((m - n) != 0)
            return -3;
    }
    return wr_bytes;
}
/*====================================================================*/
void send_response(char *msg, int len)
{
    printf("Content-Type: text/html\r\n"
            "\r\n");

    head_html();
    printf( "   <h3>%s</h3>\n"
            "   <p> %d bytes</p>\n"
            "   <hr>\n"
            "   <form action=\"upload_file.exe\" enctype=\"multipart/form-data\" method=\"post\">\n"
            "    <p>\n"
            "      <input type=\"file\" name=\"filename\"><br>\n"
            "      <input type=\"submit\" value=\"Upload\">\n"
            "    </p>\n"
            "   </form>\n"
            "   <hr>\n"
            "   %s\n"
            " </body>\n"
            "</html>", msg, len, s_time);

    exit(0);
}
/*====================================================================*/
void send_error(char *status, int len)
{
    printf("Content-Type: text/html\r\n"
            "Status: %s\r\n"
            "\r\n", status);

    head_html();
    printf( "   <h3>%s</h3>\n"
            "   <p> %d bytes</p>\n"
            "   <hr>\n"
            "   <form action=\"upload_file.exe\" enctype=\"multipart/form-data\" method=\"post\">\n"
            "    <p>\n"
            "      What files are you sending?<br>\n"
            "      <input type=\"file\" name=\"filename\"><br>\n"
            "      <input type=\"submit\" value=\"Upload\">\n"
            "    </p>\n"
            "   </form>\n"
            "   <hr>\n"
            "   %s\n"
            " </body>\n"
            "</html>", status, len, s_time);
    exit(1);
}
/*====================================================================*/
int main(int argc, char *argv[])
{
    FILE *f;
    int n, lenBound;
    int wr_bytes, content_len;
    char buf[4096], head[4096];
    
    wchar_t wPath[4096];
    char boundary[256];
    
    char *cont_type;
    
    char *doc_root;
    wchar_t wDocRoot[1024];
    wchar_t wDirName[] = L".Downloads";
    
    char filename[512];
    wchar_t wFileName[1024];
    
    char *cont_len;
    time_t now = 0;
    struct tm *t;
    
    setlocale(LC_CTYPE, "");
    fflush(stdin);
    fflush(stdout);
    
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    
    now = time(NULL);

    t = gmtime(&now);
    strftime(s_time, 128, "%a, %d %b %Y %H:%M:%S GMT", t);

    if(!(doc_root = getenv("DOCUMENT_ROOT")))
    {
        fprintf(stderr, "[%s] <%s:%s:%d> DOCUMENT_ROOT ?\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        exit(EXIT_FAILURE);
    }

    if(!(cont_type = getenv("CONTENT_TYPE")))
    {
        fprintf(stderr, "[%s] <%s:%s:%d> CONTENT_TYPE ?\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }

    memcpy(boundary, "--", 3);
    if (!find_in_str(cont_type, boundary + 2, sizeof(boundary) - 2, "boundary=", ""))
    {
        fprintf(stderr, "<%s:%s:%d> not found boundary\n", __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }
    lenBound = strlen(boundary) - 2;
    
    if((cont_len = getenv("CONTENT_LENGTH")))
    {
        sscanf(cont_len, "%d", &content_len);
        if(content_len == 0)
        {
            fprintf(stderr, "<%s:%s:%d> CONTENT_LENGTH=0\n", __FILE__, __FUNCTION__, __LINE__);
            send_error("400 Bad Request", 0);
        }
    }
    else
    {
        fprintf(stderr, "[%s] <%s:%s:%d> 411 Length Required\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("411 Length Required", 0);
    }
    // read boundary from stdin
    if(read_line(buf, sizeof(buf), &content_len) <= 0)
    {
        fprintf(stderr, "[%s] <%s:%s:%d> 400 Bad Request\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }

    n = strlen(buf) - 2;
    buf[n] = 0;

    if(memcmp(boundary, buf, lenBound + 2))
    {
        fprintf(stderr, "[%s] <%s:%s:%d> 400 Bad Request\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }

    //------------- read from stdin -------------
    n = read_head(head, sizeof(head), &content_len);
    if(n < 0)
    {
        fprintf(stderr, "[%s] <%s:%s:%d> 400 Bad Request\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }
    else if(n == 0)
    {
        send_error("400 Bad Request", 0);
    }

    find_in_str(head, filename, sizeof(filename), "filename=\"", "\"");
    if(filename[0] == 0)
    {
        fprintf(stderr, "[%s] <%s:%s:%d> filename not found\n", s_time, __FILE__, __FUNCTION__, __LINE__);
        send_error("400 Bad Request", 0);
    }
    else
    {
        n = utf8_to_utf16(wDocRoot, sizeof(wDocRoot)/sizeof(wchar_t), doc_root);
        if (n)
        {
            fprintf(stderr, "[%s] <%s:%s:%d> Error utf8_to_utf16()\n", s_time, __FILE__, __FUNCTION__, __LINE__);
            send_error("500 Internal Server Error", 0);
        }
        
        n = utf8_to_utf16(wFileName, sizeof(wFileName)/sizeof(wchar_t), filename);
        if (n)
        {
            fprintf(stderr, "[%s] <%s:%s:%d> Error utf8_to_utf16()\n", s_time, __FILE__, __FUNCTION__, __LINE__);
            send_error("500 Internal Server Error", 0);
        }
        
        _snwprintf(wPath, sizeof(wPath)/sizeof(wchar_t), L"%s/%s/%s", wDocRoot, wDirName, wFileName);
        f = _wfopen(wPath, L"wb");
        if (!f)
        {
            fprintf(stderr, "[%s] <%s:%s:%d> Error _wfopen()\n", s_time, __FILE__, __FUNCTION__, __LINE__);
            send_error("500 Internal Server Error", 0);
        }
        
        fprintf(stderr, "[%s]-<%s:%s:%d> %s\n", s_time, __FILE__, __FUNCTION__, __LINE__, filename);
        fflush(stderr);
        //------------- read from stdin -------------
        wr_bytes = write_to_file(f, content_len - 6 - strlen(boundary), &content_len);
        if(wr_bytes < 0)
        {
            fclose(f);
            fprintf(stderr, "[%s] <%s:%s:%d> Error write_to_file()\n", s_time, __FILE__, __FUNCTION__, __LINE__);
            send_error("500 Internal Server Error", 0);
        }

        n = fread(buf, 1, 6 + strlen(boundary), stdin);
        content_len -= n;
        fclose(f);
    }
/*..........................  ......................*/
    send_response(filename, wr_bytes);
    exit(EXIT_SUCCESS);
}
