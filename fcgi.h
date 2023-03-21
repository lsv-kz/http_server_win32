#ifndef FCGI_SERVER_
#define FCGI_SERVER_

#include <iostream>
#include <cstdio>
#include <poll.h>

#include "String.h"

#define FCGI_RESPONDER  1

#define FCGI_VERSION_1           1
#define FCGI_BEGIN_REQUEST       1
#define FCGI_ABORT_REQUEST       2
#define FCGI_END_REQUEST         3
#define FCGI_PARAMS              4
#define FCGI_STDIN               5
#define FCGI_STDOUT              6
#define FCGI_STDERR              7
#define FCGI_DATA                8
#define FCGI_GET_VALUES          9
#define FCGI_GET_VALUES_RESULT  10
#define FCGI_UNKNOWN_TYPE       11
#define FCGI_MAXTYPE            (FCGI_UNKNOWN_TYPE)
#define requestId               1

const int FCGI_SIZE_BUF = 4096;

int write_to_script(int fd, const char *buf, int len, int timeout);
int fcgi_set_header(char *p, int type, int len, int make_padd);
//======================================================================
typedef struct {
    unsigned char type;
    int len;
    unsigned char paddingLen;
} fcgi_header;

class FCGI_class
{
    int err = 0;
    char fcgi_buf[FCGI_SIZE_BUF];
    const char *str_zero = "\0\0\0\0\0\0\0\0";
    int offset_out;
    char *ptr_buf;
    int fcgi_sock;
    
    int numPar = 0;
    
    void send_par(int end)
    {
        if (err) return;
        int padd = fcgi_set_header(fcgi_buf, FCGI_PARAMS, offset_out, 1);
        if (padd) memcpy(ptr_buf + offset_out, str_zero, padd);
        offset_out += padd;
        if (end)
        {
            char s[8] = {1, 4, 0, 1, 0, 0, 0, 0};
            memcpy(ptr_buf + offset_out, s, 8);
            offset_out += 8;
            end = 0;
        }

        fcgi_send_buf();

        if (end)
        {
            send_par(0);
        }
    }
    
    void fcgi_send_begin()
    {
        if (err) return;
        ptr_buf[offset_out++] = (unsigned char) ((FCGI_RESPONDER >> 8) & 0xff);
        ptr_buf[offset_out++] = (unsigned char) (FCGI_RESPONDER        & 0xff);
        ptr_buf[offset_out++] = 0;//  FCGI_KEEP_CONN : 0
        ptr_buf[offset_out++] = 0;
        ptr_buf[offset_out++] = 0;
        ptr_buf[offset_out++] = 0;
        ptr_buf[offset_out++] = 0;
        ptr_buf[offset_out++] = 0;
        fcgi_set_header(fcgi_buf, FCGI_BEGIN_REQUEST, offset_out, 0);
        
        fcgi_send_buf();
    }
    
    void fcgi_send_buf()
    {
        if (write_to_script(fcgi_sock, fcgi_buf, offset_out + 8, conf->TimeoutCGI) <= 0)
            err = 1;
        offset_out = 0;
    }
    
    FCGI_class() {}
public://---------------------------------------------------------------
    FCGI_class(int s)
    {
        ptr_buf = fcgi_buf + 8;
        fcgi_sock = s;
        offset_out = 0;
        err = 0;
        fcgi_send_begin();
    }
    
    void add(const char *name, const char *val);
    void send_par(const char *name, const char *val);
    
    int error() const { return err; }
    void set_error() { err = 1; }
};
//======================================================================
    void FCGI_class::add(const char *name, const char *val)
    {
        if (err) return;
        ++numPar;
        if (!name)
        {
            send_par(1);
            return;
        }

        size_t len_name = strlen(name), len_val, len;
        
        if (val)
            len_val = strlen(val);
        else
            len_val = 0;
        
        len = len_name + len_val;
        len += len_name > 127 ? 4 : 1;
        len += len_val > 127 ? 4 : 1;
        
        if ((offset_out + len + 16) > FCGI_SIZE_BUF)
        {
            send_par(0);
            if (err)
                return;
            if ((len + 16) > FCGI_SIZE_BUF)
            {
                std::cerr << "[" << name << "  " << val << "] len=" << len <<  "\n";
                err = 1;
                return;
            }
        }
        
        char *p = ptr_buf + offset_out;
        if (len_name < 0x80)
            *p++ = (unsigned char)len_name;
        else
        {
            *p++ = (unsigned char)((len_name >> 24) | 0x80);
            *p++ = (unsigned char)(len_name >> 16);
            *p++ = (unsigned char)(len_name >> 8);
            *p++ = (unsigned char)len_name;
        }
    
        if (len_val < 0x80)
            *p++ = (unsigned char)len_val;
        else
        {
            *p++ = (unsigned char)((len_val >> 24) | 0x80);
            *p++ = (unsigned char)(len_val >> 16);
            *p++ = (unsigned char)(len_val >> 8);
            *p++ = (unsigned char)len_val;
        }
        
        memcpy(p, name, len_name);
        if (len_val > 0)
            memcpy(p + len_name, val, len_val);
        
        offset_out += len;
    }
//----------------------------------------------------------------------
    void FCGI_class::send_par(const char *name, const char *val)
    {
        if (err) return;
        ++numPar;
        if (!name)
        {
            send_par(0);
            return;
        }

        size_t len_name = strlen(name), len_val, len;
        
        if (val)
            len_val = strlen(val);
        else
            len_val = 0;

        len = len_name + len_val;
        len += len_name > 127 ? 4 : 1;
        len += len_val > 127 ? 4 : 1;
        
        if ((offset_out + len + 16) > FCGI_SIZE_BUF)
        {
            send_par(0);
            if (err)
                return;
            if ((len + 16) > FCGI_SIZE_BUF)
            {
                std::cerr << "[" << name << "  " << val << "] len=" << len <<  "\n";
                err = 1;
                return;
            }
        }

        char *p = ptr_buf + offset_out;
        if (len_name < 0x80)
            *p++ = (unsigned char)len_name;
        else
        {
            *p++ = (unsigned char)((len_name >> 24) | 0x80);
            *p++ = (unsigned char)(len_name >> 16);
            *p++ = (unsigned char)(len_name >> 8);
            *p++ = (unsigned char)len_name;
        }
    
        if (len_val < 0x80)
            *p++ = (unsigned char)len_val;
        else
        {
            *p++ = (unsigned char)((len_val >> 24) | 0x80);
            *p++ = (unsigned char)(len_val >> 16);
            *p++ = (unsigned char)(len_val >> 8);
            *p++ = (unsigned char)len_val;
        }
        
        memcpy(p, name, len_name);
        if (len_val > 0)
            memcpy(p + len_name, val, len_val);
        
        offset_out += len;
        send_par(0);
    }

#endif
