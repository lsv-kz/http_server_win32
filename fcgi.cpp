#include "main.h"

using namespace std;
//======================================================================
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
//======================================================================
int get_sock_fcgi(Connect *r, const wchar_t *script);
void cgi_del_from_list(Connect *r);
int cgi_set_size_chunk(Connect *r);
int cgi_find_empty_line(Connect *req);
int read_padding(Connect *r);
int write_to_fcgi(Connect* r);
int fcgi_read_header(Connect* r);
//======================================================================
void fcgi_set_header(Connect* r, int type)
{
    r->fcgi.fcgi_type = type;
    r->fcgi.paddingLen = 0;
    char *p = r->cgi.buf;
    *p++ = FCGI_VERSION_1;
    *p++ = (unsigned char)type;
    *p++ = (unsigned char) ((1 >> 8) & 0xff);
    *p++ = (unsigned char) ((1) & 0xff);
    *p++ = (unsigned char) ((r->fcgi.dataLen >> 8) & 0xff);
    *p++ = (unsigned char) ((r->fcgi.dataLen) & 0xff);
    *p++ = r->fcgi.paddingLen;
    *p = 0;

    r->cgi.p = r->cgi.buf;
    r->cgi.len_buf += 8;
}
//======================================================================
void fcgi_set_param(Connect *r)
{
    r->cgi.len_buf = 0;
    r->cgi.p = r->cgi.buf + 8;

    for ( ; r->fcgi.i_param < r->fcgi.size_par; ++r->fcgi.i_param)
    {
        int len_name = r->fcgi.vPar[r->fcgi.i_param].name.size();
        int len_val = r->fcgi.vPar[r->fcgi.i_param].val.size();
        int len = len_name + len_val;
        len += len_name > 127 ? 4 : 1;
        len += len_val > 127 ? 4 : 1;
        if (len > (r->cgi.size_buf - r->cgi.len_buf))
        {
            break;
        }

        if (len_name < 0x80)
            *(r->cgi.p++) = (unsigned char)len_name;
        else
        {
            *(r->cgi.p++) = (unsigned char)((len_name >> 24) | 0x80);
            *(r->cgi.p++) = (unsigned char)(len_name >> 16);
            *(r->cgi.p++) = (unsigned char)(len_name >> 8);
            *(r->cgi.p++) = (unsigned char)len_name;
        }

        if (len_val < 0x80)
            *(r->cgi.p++) = (unsigned char)len_val;
        else
        {
            *(r->cgi.p++) = (unsigned char)((len_val >> 24) | 0x80);
            *(r->cgi.p++) = (unsigned char)(len_val >> 16);
            *(r->cgi.p++) = (unsigned char)(len_val >> 8);
            *(r->cgi.p++) = (unsigned char)len_val;
        }

        memcpy(r->cgi.p, r->fcgi.vPar[r->fcgi.i_param].name.c_str(), len_name);
        r->cgi.p += len_name;
        if (len_val > 0)
        {
            memcpy(r->cgi.p, r->fcgi.vPar[r->fcgi.i_param].val.c_str(), len_val);
            r->cgi.p += len_val;
        }

        r->cgi.len_buf += len;
    }

    r->fcgi.dataLen = r->cgi.len_buf;
    fcgi_set_header(r, FCGI_PARAMS);
}
//======================================================================
int get_sock_fcgi(Connect* req, const wchar_t* script)
{
    int len;
    fcgi_list_addr* ps = conf->fcgi_list;

    if (!script)
    {
        print_err(req, "<%s:%d> Not found\n", __func__, __LINE__);
        return -RS404;
    }

    len = wcslen(script);
    if (len > 64)
    {
        print_err(req, "<%s:%d> Error len name script\n", __func__, __LINE__);
        return -RS400;
    }

    for (; ps; ps = ps->next)
    {
        if (!wcscmp(script, ps->script_name.c_str()))
            break;
    }

    if (ps != NULL)
    {
        String str;
        utf16_to_utf8(ps->addr, str);
        req->fcgi.fd = create_fcgi_socket(req, str.c_str());
        if (req->fcgi.fd == INVALID_SOCKET)
        {
            print_err(req, "<%s:%d> Error create_client_socket()\n", __func__, __LINE__);
            return -RS502;
        }
    }
    else
    {
        print_err(req, "<%s:%d> Not found\n", __func__, __LINE__);
        return -RS404;
    }

    return 0;
}
//======================================================================
int fcgi_create_connect(Connect *req)
{
    if (req->reqMethod == M_POST)
    {
        if (req->req_hdrs.iReqContentType < 0)
        {
            print_err(req, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }

        if (req->req_hdrs.reqContentLength < 0)
        {
            print_err(req, "<%s:%d> 411 Length Required\n", __func__, __LINE__);
            return -RS411;
        }

        if (req->req_hdrs.reqContentLength > conf->ClientMaxBodySize)
        {
            print_err(req, "<%s:%d> 413 Request entity too large: %lld\n", __func__, __LINE__, req->req_hdrs.reqContentLength);
            return -RS413;
        }
    }

    if (req->scriptType == PHPFPM)
    {
        req->fcgi.fd = create_fcgi_socket(req, conf->pathPHP_FPM.c_str());
        if (req->fcgi.fd == INVALID_SOCKET)
            return -RS502;
        else
            return 0;
    }
    else if (req->scriptType == FASTCGI)
        return get_sock_fcgi(req, req->wScriptName.c_str());
    else
    {
        print_err(req, "<%s:%d> ? req->scriptType=%d\n", __func__, __LINE__, req->scriptType);
        return -RS500;
    }

    return -1;
}
//======================================================================
void fcgi_create_param(Connect *req)
{
    int i = 0;
    Param param;
    req->fcgi.vPar.clear();

    if (req->scriptType == PHPFPM)
    {
        param.name = "REDIRECT_STATUS";
        param.val = "true";
        req->fcgi.vPar.push_back(param);
        ++i;
    }
/*
    {
        const int size = 4096;
        char tmpBuf[size];
        if (ExpandEnvironmentStringsA("PATH=%PATH%", tmpBuf, size))
        {
            param.name = "PATH";
            param.val = tmpBuf;
            ++i;
        }
        else
        {
            print_err(r, "<%s:%d> Error ExpandEnvironmentStringsA()\n", __func__, __LINE__);
        }
    }
*/
    param.name = "SERVER_SOFTWARE";
    param.val = conf->ServerSoftware;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "GATEWAY_INTERFACE";
    param.val = "CGI/1.1";
    req->fcgi.vPar.push_back(param);
    ++i;

    String str;
    utf16_to_utf8(conf->wRootDir, str);
    param.name = "DOCUMENT_ROOT";
    param.val = str;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REMOTE_ADDR";
    param.val = req->remoteAddr;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REMOTE_PORT";
    param.val = req->remotePort;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REQUEST_URI";
    param.val = req->uri;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REQUEST_METHOD";
    param.val = get_str_method(req->reqMethod);
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SERVER_PROTOCOL";
    param.val = get_str_http_prot(req->httpProt);
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SERVER_PORT";
    param.val = conf->ServerPort;
    req->fcgi.vPar.push_back(param);
    ++i;

    if (req->req_hdrs.iHost >= 0)
    {
        param.name = "HTTP_HOST";
        param.val = req->req_hdrs.Value[req->req_hdrs.iHost];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->req_hdrs.iReferer >= 0)
    {
        param.name = "HTTP_REFERER";
        param.val = req->req_hdrs.Value[req->req_hdrs.iReferer];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->req_hdrs.iUserAgent >= 0)
    {
        param.name = "HTTP_USER_AGENT";
        param.val = req->req_hdrs.Value[req->req_hdrs.iUserAgent];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    param.name = "HTTP_CONNECTION";
    if (req->connKeepAlive == 1)
        param.val = "keep-alive";
    else
        param.val = "close";
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SCRIPT_NAME";
    param.val = req->decodeUri;
    req->fcgi.vPar.push_back(param);
    ++i;

    if (req->scriptType == PHPFPM)
    {
        utf16_to_utf8(conf->wRootDir + req->wScriptName, str);
        param.name = "SCRIPT_FILENAME";
        param.val = str;
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->reqMethod == M_POST)
    {
        if (req->req_hdrs.iReqContentType >= 0)
        {
            param.name = "CONTENT_TYPE";
            param.val = req->req_hdrs.Value[req->req_hdrs.iReqContentType];
            req->fcgi.vPar.push_back(param);
            ++i;
        }

        if (req->req_hdrs.iReqContentLength >= 0)
        {
            param.name = "CONTENT_LENGTH";
            param.val = req->req_hdrs.Value[req->req_hdrs.iReqContentLength];
            req->fcgi.vPar.push_back(param);
            ++i;
        }
    }

    param.name = "QUERY_STRING";
    if (req->sReqParam)
        param.val = req->sReqParam;
    else
        param.val = "";
    req->fcgi.vPar.push_back(param);
    ++i;

    if (i != (int)req->fcgi.vPar.size())
    {
        print_err(req, "<%s:%d> Error: create fcgi param list\n", __func__, __LINE__);
    }

    req->fcgi.size_par = i;
    req->fcgi.i_param = 0;
    //----------------------------------------------
    req->fcgi.dataLen = req->cgi.len_buf = 8;
    fcgi_set_header(req, FCGI_BEGIN_REQUEST);
    char *p = req->cgi.buf + 8;
    *(p++) = (unsigned char) ((FCGI_RESPONDER >> 8) & 0xff);
    *(p++) = (unsigned char) (FCGI_RESPONDER        & 0xff);
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;

    req->cgi.status.fcgi = FASTCGI_BEGIN;
    req->io_direct = TO_CGI;
    req->sock_timer = 0;
    req->fcgi.http_headers_received = false;
}
//======================================================================
int fcgi_stdin(Connect *r)
{
    if (r->io_direct == FROM_CLIENT)
    {
        int rd = (r->cgi.len_post > r->cgi.size_buf) ? r->cgi.size_buf : r->cgi.len_post;
        r->cgi.len_buf = recv(r->clientSocket, r->cgi.buf + 8, rd, 0);
        if (r->cgi.len_buf == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
            return -1;
        }
        else if (r->cgi.len_buf == 0)
        {
            print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
            return -1;
        }

        r->cgi.len_post -= r->cgi.len_buf;
        r->fcgi.dataLen = r->cgi.len_buf;
        fcgi_set_header(r, FCGI_STDIN);
        r->io_direct = TO_CGI;
    }
    else if (r->io_direct == TO_CGI)
    {
        int n = send(r->fcgi.fd, r->cgi.p, r->cgi.len_buf, 0);
        if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
            return -1;
        }

        r->cgi.p += n;
        r->cgi.len_buf -= n;
        if (r->cgi.len_buf == 0)
        {
            if (r->cgi.len_post <= 0)
            {
                if (r->fcgi.dataLen == 0)
                {
                    r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                    r->fcgi.len_header = 0;
                    r->p_newline = r->cgi.p = r->cgi.buf + 8;
                    r->cgi.len_buf = 0;
                    r->tail = NULL;
                    r->io_direct = FROM_CGI;
                }
                else
                {
                    r->cgi.len_buf = 0;
                    r->fcgi.dataLen = r->cgi.len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    //r->io_direct = TO_CGI;
                }
            }
            else
            {
                if (r->lenTail > 0)
                {
                    if (r->lenTail > r->cgi.size_buf)
                        r->cgi.len_buf = r->cgi.size_buf;
                    else
                        r->cgi.len_buf = r->lenTail;
                    memcpy(r->cgi.buf + 8, r->tail, r->cgi.len_buf);
                    r->lenTail -= r->cgi.len_buf;
                    r->cgi.len_post -= r->cgi.len_buf;
                    if (r->lenTail == 0)
                        r->tail = NULL;
                    else
                        r->tail += r->cgi.len_buf;
                    r->fcgi.dataLen = r->cgi.len_buf;
                    fcgi_set_header(r, FCGI_STDIN);
                    //r->io_direct = TO_CGI;
                }
                else
                {
                    r->io_direct = FROM_CLIENT;
                }
            }
        }
    }

    return 0;
}
//======================================================================
int fcgi_stdout(Connect *r)
{
    if (r->io_direct == FROM_CGI)
    {
        if ((r->cgi.status.fcgi == FASTCGI_SEND_ENTITY) ||
            (r->cgi.status.fcgi == FASTCGI_READ_ERROR) ||
            (r->cgi.status.fcgi == FASTCGI_CLOSE))
        {
            if (r->fcgi.dataLen == 0)
            {
                r->cgi.status.fcgi = FASTCGI_READ_PADDING;
                return 1;
            }

            int len = (r->fcgi.dataLen > r->cgi.size_buf) ? r->cgi.size_buf : r->fcgi.dataLen;
            r->cgi.len_buf = recv(r->fcgi.fd, r->cgi.buf + 8, len, 0);
            if (r->cgi.len_buf == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                    return TRYAGAIN;
                r->err = -1;
                return -1;
            }
            else if (r->cgi.len_buf == 0)
            {
                print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
                r->err = -1;
                return -1;
            }

            if (r->cgi.status.fcgi == FASTCGI_SEND_ENTITY)
            {
                r->io_direct = TO_CLIENT;
                r->fcgi.dataLen -= r->cgi.len_buf;
                if (r->mode_send == CHUNK)
                {
                    if (cgi_set_size_chunk(r))
                        return -1;
                }
                else
                    r->cgi.p = r->cgi.buf + 8;
            }
            else if (r->cgi.status.fcgi == FASTCGI_READ_ERROR)
            {
                r->fcgi.dataLen -= r->cgi.len_buf;
                if (r->fcgi.dataLen == 0)
                {
                    r->cgi.status.fcgi = FASTCGI_READ_PADDING;
                }
                *(r->cgi.buf + 8 + r->cgi.len_buf) = 0;
                print_err("%s\n", r->cgi.buf + 8);
            }
            else if (r->cgi.status.fcgi == FASTCGI_CLOSE)
            {
                r->fcgi.dataLen -= r->cgi.len_buf;
                if (r->fcgi.dataLen == 0)
                {
                    if (r->mode_send == NO_CHUNK)
                    {
                        r->connKeepAlive = 0;
                        return 0;
                    }
                    else
                    {
                        r->mode_send = CHUNK_END;
                        r->cgi.len_buf = 0;
                        cgi_set_size_chunk(r);
                        r->io_direct = TO_CLIENT;
                        r->sock_timer = 0;
                    }
                }
            }
        }
        else if (r->cgi.status.fcgi == FASTCGI_READ_PADDING)
        {
            if (r->fcgi.paddingLen > 0)
            {
                char buf[256];

                int len = (r->fcgi.paddingLen > (int)sizeof(buf)) ? sizeof(buf) : r->fcgi.paddingLen;
                int n = recv(r->fcgi.fd, buf, len, 0);
                if (n == SOCKET_ERROR)
                {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                        return TRYAGAIN;
                    r->err = -1;
                    return -1;
                }
                else if (n == 0)
                {
                    print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
                }
                else
                {
                    r->fcgi.paddingLen -= n;
                }
            }

            if (r->fcgi.paddingLen == 0)
            {
                r->sock_timer = 0;
                r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                r->io_direct = FROM_CGI;
                r->fcgi.len_header = 0;
            }
        }
    }
    else if (r->io_direct == TO_CLIENT)
    {
        int ret = send(r->clientSocket, r->cgi.p, r->cgi.len_buf, 0);
        if (ret == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
            r->err = -1;
            return -1;
        }

        r->cgi.p += ret;
        r->cgi.len_buf -= ret;
        r->resp.send_bytes += ret;
        if (r->cgi.len_buf == 0)
        {
            if (r->cgi.status.fcgi == FASTCGI_CLOSE)
                return 0;

            if (r->fcgi.dataLen == 0)
            {
                if (r->fcgi.paddingLen == 0)
                {
                    r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                    r->fcgi.len_header = 0;
                    r->cgi.p = r->cgi.buf + 8;
                    r->cgi.len_buf = 0;
                }
                else
                {
                    r->cgi.status.fcgi = FASTCGI_READ_PADDING;
                }
            }

            r->io_direct = FROM_CGI;
        }
    }
    return 1;
}
//======================================================================
int fcgi_read_http_headers(Connect *r)
{
    int num_read;
    if ((r->cgi.size_buf - r->cgi.len_buf - 1) >= r->fcgi.dataLen)
        num_read = r->fcgi.dataLen;
    else
        num_read = r->cgi.size_buf - r->cgi.len_buf - 1;
    if (num_read <= 0)
    {
        r->err = -RS502;
        return -1;
    }

    int n = recv(r->fcgi.fd, r->cgi.p, num_read, 0);
    if (n == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return TRYAGAIN;
        r->err = -RS502;
        return -1;
    }
    else if (n == 0)
    {
        print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
        r->err = -RS502;
        return -1;
    }

    r->fcgi.dataLen -= n;
    r->lenTail += n;
    r->cgi.len_buf += n;
    r->cgi.p += n;
    *(r->cgi.p) = 0;

    n = cgi_find_empty_line(r);
    if (n == 1) // empty line found
    {
        return r->cgi.len_buf;
    }
    else if (n < 0) // error
    {
        r->err = -RS502;
        return n;
    }

    r->sock_timer = 0;
    return 0;
}
//======================================================================
void fcgi_worker(Connect* r)
{
    if (r->cgi.status.fcgi == FASTCGI_CONNECT)
    {
        if (r->io_status == WORK)
            fcgi_create_param(r);
    }
    else if (r->cgi.status.fcgi == FASTCGI_BEGIN)
    {
        int ret = write_to_fcgi(r);
        if (ret > 0)
        {
            r->sock_timer = 0;
            if (r->cgi.len_buf == 0)
            {
                r->cgi.status.fcgi = FASTCGI_PARAMS;
            }
        }
        else if (ret < 0)
        {
            if (ret == TRYAGAIN)
                r->io_status = SELECT;
            else
            {
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi.status.fcgi == FASTCGI_PARAMS)
    {
        if (r->cgi.len_buf == 0)
        {
            fcgi_set_param(r);
        }

        int ret = write_to_fcgi(r);
        if (ret > 0)
        {
            r->sock_timer = 0;
            if ((r->cgi.len_buf == 0) && (r->fcgi.dataLen == 0)) // end params
            {
                r->cgi.status.fcgi = FASTCGI_STDIN;
                r->io_direct = FROM_CLIENT;
                if (r->req_hdrs.reqContentLength > 0)
                {
                    r->cgi.len_post = r->req_hdrs.reqContentLength;
                    if (r->lenTail > 0)
                    {
                        if (r->lenTail > r->cgi.size_buf)
                        r->cgi.len_buf = r->cgi.size_buf;
                        else
                            r->cgi.len_buf = r->lenTail;
                        memcpy(r->cgi.buf + 8, r->tail, r->cgi.len_buf);
                        r->lenTail -= r->cgi.len_buf;
                        r->cgi.len_post -= r->cgi.len_buf;
                        if (r->lenTail == 0)
                            r->tail = NULL;
                        else
                            r->tail += r->cgi.len_buf;
                        r->fcgi.dataLen = r->cgi.len_buf;
                        fcgi_set_header(r, FCGI_STDIN);
                        r->io_direct = TO_CGI;
                    }
                    else
                    {
                        r->io_direct = FROM_CLIENT;
                    }
                }
                else
                {
                    r->cgi.len_post = 0;
                    r->cgi.len_buf = 0;
                    r->fcgi.dataLen = r->cgi.len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    r->io_direct = TO_CGI;
                }
            }
        }
        else if (ret < 0)
        {
            if (ret == TRYAGAIN)
                r->io_status = SELECT;
            else
            {
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi.status.fcgi == FASTCGI_STDIN)
    {
        int ret = fcgi_stdin(r);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
                r->io_status = SELECT;
            else
            {
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else
        {
            if ((r->cgi.len_buf == 0) && (r->fcgi.dataLen == 0)) //end post data
            {
                r->sock_timer = 0;
                r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                r->io_direct = FROM_CGI;
                r->fcgi.dataLen = 0;
                r->fcgi.paddingLen = 0;
                r->fcgi.len_header = 0;
                r->tail = NULL;
                r->p_newline = r->cgi.p = r->cgi.buf + 8;
                r->cgi.len_buf = 0;
                return;
            }
        }
    }
    else //====================== FCGI_STDOUT===========================
    {
        if (r->cgi.status.fcgi == FASTCGI_READ_HEADER)
        {
            int ret = fcgi_read_header(r);
            if (ret == 8)
            {
                switch (r->fcgi.fcgi_type)
                {
                    case FCGI_STDOUT:
                    {
                        if (r->fcgi.http_headers_received == true)
                        {
                            if (r->fcgi.dataLen > 0)
                            {
                                r->cgi.status.fcgi = FASTCGI_SEND_ENTITY;
                            }
                            else
                            {
                                r->cgi.status.fcgi = FASTCGI_READ_PADDING;
                            }
                        }
                        else
                        {
                            r->cgi.status.fcgi = FASTCGI_READ_HTTP_HEADERS;
                            r->io_direct = FROM_CGI;
                        }
                    }
                    break;
                    case FCGI_STDERR:
                        r->cgi.status.fcgi = FASTCGI_READ_ERROR;
                        break;
                    case FCGI_END_REQUEST:
                        r->cgi.status.fcgi = FASTCGI_CLOSE;
                        if (r->fcgi.dataLen > 0)
                        {
                            r->io_direct = FROM_CGI;
                        }
                        break;
                    default:
                        print_err(r, "<%s:%d> Error type=%d\n", __func__, __LINE__, r->fcgi.fcgi_type);
                        r->err = -1;
                        cgi_del_from_list(r);
                        end_response(r);
                }
            }
            else if (ret < 0)
            {
                if (ret == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    print_err(r, "<%s:%d> Error fcgi_read_header()=%d\n", __func__, __LINE__, ret);
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else
                r->sock_timer = 0;
        }
        else if (r->cgi.status.fcgi == FASTCGI_READ_PADDING)
        {
            int ret = read_padding(r);
            if (ret < 0)
            {
                if (ret == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else
                r->sock_timer = 0;
        }
        else if (r->cgi.status.fcgi == FASTCGI_READ_HTTP_HEADERS)
        {
            int ret = fcgi_read_http_headers(r);
            if (ret > 0)
            {
                r->mode_send = ((r->httpProt == HTTP11) && r->connKeepAlive) ? CHUNK : NO_CHUNK;
                if (create_response_headers(r))
                {
                    print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
                else
                {
                    r->fcgi.http_headers_received = true;
                    r->resp_headers.p = r->resp_headers.s.c_str();
                    r->resp_headers.len = r->resp_headers.s.size();
                    r->cgi.status.fcgi = FASTCGI_SEND_HTTP_HEADERS;
                    r->io_direct = TO_CLIENT;
                    r->sock_timer = 0;
                }
            }
            else if (ret < 0)
            {
                if (ret == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    r->err = -RS502;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else
            {
                if (r->fcgi.dataLen == 0)
                {
                    if (r->fcgi.paddingLen == 0)
                    {
                        r->sock_timer = 0;
                        r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                        r->io_direct = FROM_CGI;
                        r->fcgi.len_header = 0;
                    }
                    else
                        r->cgi.status.fcgi = FASTCGI_READ_PADDING;
                }
            }
        }
        else if (r->cgi.status.fcgi == FASTCGI_SEND_HTTP_HEADERS)
        {
            if (r->resp_headers.len > 0)
            {
                int wr = send(r->clientSocket, r->resp_headers.p, r->resp_headers.len, 0);
                if (wr == SOCKET_ERROR)
                {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                    {
                        r->io_status = SELECT;
                        return;
                    }
                    r->err = -1;
                    r->req_hdrs.iReferer = MAX_HEADERS - 1;
                    r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                    cgi_del_from_list(r);
                    end_response(r);
                }
                else
                {
                    r->resp_headers.p += wr;
                    r->resp_headers.len -= wr;
                    if (r->resp_headers.len == 0)
                    {
                        if (r->reqMethod == M_HEAD)
                        {
                            cgi_del_from_list(r);
                            end_response(r);
                        }
                        else
                        {
                            r->cgi.status.fcgi = FASTCGI_SEND_ENTITY;
                            r->sock_timer = 0;
                            if (r->lenTail > 0)
                            {
                                r->cgi.p = r->tail;
                                r->cgi.len_buf = r->lenTail;
                                r->tail = NULL;
                                r->lenTail = 0;

                                r->io_direct = TO_CLIENT;
                                r->sock_timer = 0;
                                if (r->mode_send == CHUNK)
                                {
                                    if (cgi_set_size_chunk(r))
                                    {
                                        r->err = -1;
                                        cgi_del_from_list(r);
                                        end_response(r);
                                    }
                                }
                            }
                            else
                            {
                                r->io_direct = FROM_CGI;
                                r->sock_timer = 0;
                            }
                        }
                    }
                    else
                        r->sock_timer = 0;
                }
            }
        }
        else if ((r->cgi.status.fcgi == FASTCGI_SEND_ENTITY) ||
                 (r->cgi.status.fcgi == FASTCGI_READ_ERROR) ||
                (r->cgi.status.fcgi == FASTCGI_CLOSE))
        {
            int ret = fcgi_stdout(r);
            if (ret < 0)
            {
                if (ret == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else if (ret == 0) // end
            {
                cgi_del_from_list(r);
                end_response(r);
            }
            else
                r->sock_timer = 0;
        }
        else
        {
            print_err(r, "<%s:%d> Error status=%d\n", __func__, __LINE__, r->cgi.status.fcgi);
            r->err = -1;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
}
//======================================================================
int timeout_fcgi(Connect *r)
{
    if (r->cgi.status.fcgi == FASTCGI_CONNECT)
        return -RS502;
    else if (((r->cgi.status.fcgi == FASTCGI_BEGIN) ||
         (r->cgi.status.fcgi == FASTCGI_PARAMS) ||
         (r->cgi.status.fcgi == FASTCGI_STDIN)) &&
       (r->io_direct == TO_CGI))
        return -RS504;
    else if (r->cgi.status.fcgi == FASTCGI_READ_HTTP_HEADERS)
        return -RS504;
    else
        return -1;
}
//======================================================================
int read_padding(Connect *r)
{
    if (r->fcgi.paddingLen > 0)
    {
        char buf[256];

        int len = (r->fcgi.paddingLen > (int)sizeof(buf)) ? sizeof(buf) : r->fcgi.paddingLen;
        int n = recv(r->fcgi.fd, buf, len, 0);
        if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
            return -1;
        }
        else if (n == 0)
        {
            r->err = -1;
            return -1;
        }
        else
        {
            r->fcgi.paddingLen -= n;
        }
    }

    if (r->fcgi.paddingLen == 0)
    {
        r->cgi.status.fcgi = FASTCGI_READ_HEADER;
        r->fcgi.len_header = 0;
        r->cgi.p = r->cgi.buf + 8;
        r->cgi.len_buf = 0;
        r->io_direct = FROM_CGI;
    }

    return 0;
}
