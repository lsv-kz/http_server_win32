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
const int requestId = 1;
//======================================================================
extern struct pollfd *cgi_poll_fd;

int get_sock_fcgi(Connect *r, const wchar_t *script);
void cgi_del_from_list(Connect *r);
int cgi_set_size_chunk(Connect *r);
int cgi_find_empty_line(Connect *req);
int read_padding(Connect *r);
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

    if (r->cgi.len_buf > 0)
    {
        r->fcgi.dataLen = r->cgi.len_buf;
        fcgi_set_header(r, FCGI_PARAMS);
    }
    else
    {
        r->fcgi.dataLen = r->cgi.len_buf;
        fcgi_set_header(r, FCGI_PARAMS);
    }
}
//======================================================================
SOCKET create_fcgi_socket(const char* host)
{
    char addr[256];
    char port[16] = "";
    std::string sHost = host;

    if (!host)
    {
        print_err("<%s:%d> Error: host=NULL\n", __func__, __LINE__);
        return -1;
    }

    size_t sz = sHost.find(':');
    if (sz == std::string::npos)
    {
        print_err("<%s:%d> \n", __func__, __LINE__);
        return -1;
    }

    sHost.copy(addr, sz);
    addr[sz] = 0;

    size_t len = sHost.copy(port, sHost.size() - sz + 1, sz + 1);
    port[len] = 0;
    //----------------------------------------------------------------------
    SOCKET sockfd;
    SOCKADDR_IN sock_addr;

    memset(&sock_addr, 0, sizeof(sock_addr));
    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET)
    {
        ErrorStrSock(__func__, __LINE__, "Error socket()");
        return -1;
    }

    sock_addr.sin_port = htons(atoi(port));
    sock_addr.sin_family = AF_INET;

    if (in4_aton(addr, &(sock_addr.sin_addr)) != 4)
    {
        print_err("<%s:%d> Error in4_aton()=%d\n", __func__, __LINE__);
        closesocket(sockfd);
        return -1;
    }

    DWORD sock_opt = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)& sock_opt, sizeof(sock_opt)) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error setsockopt(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        return -1;
    }

    u_long iMode = 1;
    if (ioctlsocket(sockfd, FIONBIO, &iMode) == SOCKET_ERROR)
    {
        ErrorStrSock(__func__, __LINE__, "Error ioctlsocket()");
        closesocket(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)(&sock_addr), sizeof(sock_addr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        //int err = ErrorStrSock(__func__, __LINE__, "Error: connect");
        if (err != WSAEWOULDBLOCK)
        {
            print_err("<%s:%d> Error connect(): %d\n", __func__, __LINE__, err);
            closesocket(sockfd);
            return -1;
        }
    }

    return sockfd;
}
//======================================================================
int get_sock_fcgi(Connect* req, const wchar_t* script)
{
    int fcgi_sock = -1, len;
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
        fcgi_sock = create_fcgi_socket(str.c_str());
        if (fcgi_sock < 0)
        {
            print_err(req, "<%s:%d> Error create_client_socket()\n", __func__, __LINE__);
            fcgi_sock = -RS500;
        }
    }
    else
    {
        print_err(req, "<%s:%d> Not found\n", __func__, __LINE__);
        fcgi_sock = -RS404;
    }

    return fcgi_sock;
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

    if (req->resp.scriptType == PHPFPM)
        req->fcgi.fd = create_fcgi_socket(conf->pathPHP_FPM.c_str());
    else if (req->resp.scriptType == FASTCGI)
        req->fcgi.fd = get_sock_fcgi(req, req->wScriptName.c_str());
    else
    {
        print_err(req, "<%s:%d> ? req->scriptType=%d\n", __func__, __LINE__, req->resp.scriptType);
        return -RS500;
    }

    if (req->fcgi.fd < 0)
    {
        return req->fcgi.fd;
    }

    return 0;
}
//======================================================================
void fcgi_create_param(Connect *req)
{
    int i = 0;
    Param param;
    req->fcgi.vPar.clear();

    if (req->resp.scriptType == PHPFPM)
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

    if (req->resp.scriptType == PHPFPM)
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
    req->cgi.dir = TO_CGI;
    req->timeout = conf->TimeoutCGI;
    req->sock_timer = 0;
    req->fcgi.http_headers_received = false;
}
//======================================================================
int fcgi_stdin(Connect *r)
{
    if (r->cgi.dir == FROM_CLIENT)
    {
        int rd = (r->cgi.len_post > r->cgi.size_buf) ? r->cgi.size_buf : r->cgi.len_post;
        r->cgi.len_buf = recv(r->clientSocket, r->cgi.buf + 8, rd, 0);
        if (r->cgi.len_buf == SOCKET_ERROR)
        {
            int err = GetLastError();
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
        r->cgi.dir = TO_CGI;
        r->timeout = conf->TimeoutCGI;
    }
    else if (r->cgi.dir == TO_CGI)
    {
        int n = send(r->fcgi.fd, r->cgi.p, r->cgi.len_buf, 0);
        if (n == SOCKET_ERROR)
        {
            int err = GetLastError();
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
                    r->cgi.dir = FROM_CGI;
                    //r->timeout = conf->TimeoutCGI;
                }
                else
                {
                    r->cgi.len_buf = 0;
                    r->fcgi.dataLen = r->cgi.len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    //r->cgi.dir = TO_CGI;
                    //r->timeout = conf->TimeoutCGI;
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
                    //r->cgi.dir = TO_CGI;
                }
                else
                {
                    r->cgi.dir = FROM_CLIENT;
                    r->timeout = conf->TimeOut;
                }
            }
        }
    }

    return 0;
}
//======================================================================
int fcgi_stdout(Connect *r)
{
    if (r->cgi.dir == FROM_CGI)
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
                int err = GetLastError();
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
                r->cgi.dir = TO_CLIENT;
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
                        r->cgi.dir = TO_CLIENT;
                        r->timeout = conf->TimeOut;
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
                    int err = GetLastError();
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
                r->timeout = conf->TimeoutCGI;
                r->sock_timer = 0;
                r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                r->cgi.dir = FROM_CGI;
                r->fcgi.len_header = 0;
            }
        }
    }
    else if (r->cgi.dir == TO_CLIENT)
    {
        int ret = send(r->clientSocket, r->cgi.p, r->cgi.len_buf, 0);
        if (ret == SOCKET_ERROR)
        {
            int err = GetLastError();
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

            r->cgi.dir = FROM_CGI;
            r->timeout = conf->TimeoutCGI;
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
        cgi_del_from_list(r);
        end_response(r);
        return -1;
    }

    int n = recv(r->fcgi.fd, r->cgi.p, num_read, 0);
    if (n == SOCKET_ERROR)
    {
        int err = GetLastError();
        if (err == WSAEWOULDBLOCK)
            return -WSAEWOULDBLOCK;
        r->err = -RS502;
        cgi_del_from_list(r);
        end_response(r);
        return -1;
    }
    else if (n == 0)
    {
        print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
        r->err = -RS502;
        cgi_del_from_list(r);
        end_response(r);
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
        r->cgi.status.fcgi = FASTCGI_SEND_HTTP_HEADERS;
        r->timeout = conf->TimeOut;
        r->fcgi.http_headers_received = true;
        r->sock_timer = 0;
        return r->cgi.len_buf;
    }
    else if (n < 0) // error
    {
        r->err = -RS502;
        cgi_del_from_list(r);
        end_response(r);
        return n;
    }

    r->sock_timer = 0;
    return 0;
}
//======================================================================
int write_to_fcgi(Connect* r)
{
    int ret = send(r->fcgi.fd, r->cgi.p, r->cgi.len_buf, 0);
    if (ret == SOCKET_ERROR)
    {
        int err = GetLastError();
        return -err;
    }
    else
    {
        r->cgi.len_buf -= ret;
        r->cgi.p += ret;
        r->sock_timer = 0;
    }
    
    return ret;
}
//======================================================================
int fcgi_read_header(Connect* r)
{
    int n = 0;
    if (r->fcgi.len_header < 8)
    {
        int len = 8 - r->fcgi.len_header;
        n = recv(r->fcgi.fd, r->fcgi.buf + r->fcgi.len_header, len, 0);
        if (n == 0)
        {
            print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
            return -RS502;  
        }
        else if (n == SOCKET_ERROR)
        {
            int err = GetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                //print_err(r, "<%s:%d> EAGAIN\n", __func__, __LINE__);
                return -WSAEWOULDBLOCK;
            }
            print_err(r, "<%s:%d> err=%d\n", __func__, __LINE__, err);
            return -1;
        }
    }
    
    r->fcgi.len_header += n;
    if (r->fcgi.len_header == 8)
    {
        r->fcgi.fcgi_type = (unsigned char)r->fcgi.buf[1];
        r->fcgi.paddingLen = (unsigned char)r->fcgi.buf[6];
        r->fcgi.dataLen = ((unsigned char)r->fcgi.buf[4]<<8) | (unsigned char)r->fcgi.buf[5];
    }

    return r->fcgi.len_header;
}
//======================================================================
void fcgi_set_poll_list(Connect *r, int *nsock)
{
    r->io_status = POLL;
    if (r->cgi.dir == FROM_CLIENT)
    {
        cgi_poll_fd[*nsock].fd = r->clientSocket;
        cgi_poll_fd[*nsock].events = POLLRDNORM;
    }
    else if (r->cgi.dir == TO_CLIENT)
    {
        cgi_poll_fd[*nsock].fd = r->clientSocket;
        cgi_poll_fd[*nsock].events = POLLWRNORM;
    }
    else if (r->cgi.dir == FROM_CGI)
    {
        cgi_poll_fd[*nsock].fd = r->fcgi.fd;
        cgi_poll_fd[*nsock].events = POLLRDNORM;
    }
    else if (r->cgi.dir == TO_CGI)
    {
        cgi_poll_fd[*nsock].fd = r->fcgi.fd;
        cgi_poll_fd[*nsock].events = POLLWRNORM;
    }
    (*nsock)++;
}
//======================================================================
void fcgi_worker(Connect* r)
{
    if (r->cgi.status.fcgi == FASTCGI_BEGIN)
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
        else if (ret == -WSAENOTCONN)
            return;
        else if (ret < 0)
        {
            r->err = -RS502;
            cgi_del_from_list(r);
            end_response(r);
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
                r->cgi.dir = FROM_CLIENT;
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
                        r->cgi.dir = TO_CGI;
                        r->timeout = conf->TimeoutCGI;
                    }
                    else
                    {
                        r->cgi.dir = FROM_CLIENT;
                        r->timeout = conf->TimeOut;
                    }
                }
                else
                {
                    r->cgi.len_post = 0;
                    r->cgi.len_buf = 0;
                    r->fcgi.dataLen = r->cgi.len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    r->cgi.dir = TO_CGI;
                    r->timeout = conf->TimeoutCGI;
                }
            }
        }
        else if (ret < 0)
        {
            if (ret != TRYAGAIN)
            {
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi.status.fcgi == FASTCGI_STDIN)
    {
        int n = fcgi_stdin(r);
        if (n < 0)
        {
            if (n != TRYAGAIN)
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
                r->timeout = conf->TimeoutCGI;
                r->sock_timer = 0;
                r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                r->cgi.dir = FROM_CGI;
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
                            r->timeout = conf->TimeoutCGI;
                            r->cgi.status.fcgi = FASTCGI_READ_HTTP_HEADERS;
                            r->cgi.dir = FROM_CGI;
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
                            r->cgi.dir = FROM_CGI;
                            r->timeout = conf->TimeoutCGI;
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
                if (ret != TRYAGAIN)
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
                if (ret != TRYAGAIN)
                {
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
                    r->resp_headers.p = r->resp_headers.s.c_str();
                    r->resp_headers.len = r->resp_headers.s.size();
                    r->cgi.status.fcgi = FASTCGI_SEND_HTTP_HEADERS;
                    r->timeout = conf->TimeOut;
                }
            }
            else if (ret < 0)
            {
                if (ret != TRYAGAIN)
                {
                    r->err = -RS502;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else
            {
                r->timeout = conf->TimeoutCGI;
                if (r->fcgi.dataLen == 0)
                {
                    if (r->fcgi.paddingLen == 0)
                    {
                        r->timeout = conf->TimeoutCGI;
                        r->sock_timer = 0;
                        r->cgi.status.fcgi = FASTCGI_READ_HEADER;
                        r->cgi.dir = FROM_CGI;
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
                    int err = GetLastError();
                    if (err == WSAEWOULDBLOCK)
                        return;
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
                        /*if (r->reqMethod == M_HEAD)
                        {
                            cgi_del_from_list(r);
                            end_response(r);
                        }
                        else*/
                        {
                            r->cgi.status.fcgi = FASTCGI_SEND_ENTITY;
                            r->sock_timer = 0;
                            if (r->lenTail > 0)
                            {
                                r->cgi.p = r->tail;
                                r->cgi.len_buf = r->lenTail;
                                r->tail = NULL;
                                r->lenTail = 0;
                                
                                r->cgi.dir = TO_CLIENT;
                                r->timeout = conf->TimeOut;
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
                                r->cgi.dir = FROM_CGI;
                                r->timeout = conf->TimeoutCGI;
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
                if (ret != TRYAGAIN)
                {
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
    if (((r->cgi.status.fcgi == FASTCGI_BEGIN) || 
         (r->cgi.status.fcgi == FASTCGI_PARAMS) ||
         (r->cgi.status.fcgi == FASTCGI_STDIN)) && 
       (r->cgi.dir == TO_CGI))
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
            int err = ErrorStrSock(__func__, __LINE__, "Error recv()");
            //int err = GetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
            r->err = -1;
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
        r->cgi.dir = FROM_CGI;
    }

    return 0;
}
