#include "main.h"

using namespace std;
//======================================================================
extern struct pollfd *cgi_poll_fd;
extern Connect **cgi_poll_array;

int get_sock_fcgi(Connect *r, const wchar_t *script);
void cgi_del_from_list(Connect *r);
int scgi_set_param(Connect *r);
int cgi_set_size_chunk(Connect *r);
int write_to_fcgi(Connect* r);
int scgi_read_hdrs(Connect *req);
int cgi_stdin(Connect *req);
int cgi_stdout(Connect *req);
int cgi_find_empty_line(Connect *req);
int scgi_stdout(Connect *req);
//======================================================================
int scgi_set_size_data(Connect* r)
{
    int size = r->cgi.len_buf;
    int i = 7;
    char *p = r->cgi.buf;
    p[i--] = ':';
    
    for ( ; i >= 0; --i)
    {
        p[i] = (size % 10) + '0';
        size /= 10;
        if (size == 0)
            break;
    }
    
    if (size != 0)
        return -1;

    r->cgi.buf[8 + r->cgi.len_buf] = ',';
    r->cgi.p = r->cgi.buf + i;
    r->cgi.len_buf += (8 - i + 1);

    return 0;
}
//======================================================================
int scgi_create_connect(Connect *req)
{
    req->cgi.status.scgi = SCGI_CONNECT;
    req->cgi.dir = TO_CGI;
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

    req->fcgi.fd = get_sock_fcgi(req, req->wScriptName.c_str());
    if (req->fcgi.fd < 0)
    {
        print_err(req, "<%s:%d> Error connect to scgi\n", __func__, __LINE__);
        return req->fcgi.fd;
    }

    int i = 0;
    Param param;
    req->fcgi.vPar.clear();

    param.name = "PATH";
    param.val = "/bin:/usr/bin:/usr/local/bin";
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SERVER_SOFTWARE";
    param.val = conf->ServerSoftware;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SCGI";
    param.val = "1";
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
    
    param.name = "DOCUMENT_URI";
    param.val = req->decodeUri;
    req->fcgi.vPar.push_back(param);
    ++i;

    if (req->reqMethod == M_HEAD)
    {
        param.name = "REQUEST_METHOD";
        param.val = get_str_method(M_GET);
        req->fcgi.vPar.push_back(param);
        ++i;
    }
    else
    {
        param.name = "REQUEST_METHOD";
        param.val = get_str_method(req->reqMethod);
        req->fcgi.vPar.push_back(param);
        ++i;
    }

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

    if (req->reqMethod == M_POST)
    {
        param.name = "CONTENT_TYPE";
        param.val = req->req_hdrs.Value[req->req_hdrs.iReqContentType];
        req->fcgi.vPar.push_back(param);
        ++i;

        param.name = "CONTENT_LENGTH";
        param.val = req->req_hdrs.Value[req->req_hdrs.iReqContentLength];
        req->fcgi.vPar.push_back(param);
        ++i;
    }
    else
    {
        param.name = "CONTENT_LENGTH";
        param.val = "0";
        req->fcgi.vPar.push_back(param);
        ++i;
        
        param.name = "CONTENT_TYPE";
        param.val = "";
        req->fcgi.vPar.push_back(param);
        ++i;
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
        return -1;
    }

    req->fcgi.size_par = i;
    req->fcgi.i_param = 0;
    
    req->cgi.status.scgi = SCGI_PARAMS;
    req->cgi.dir = TO_CGI;
    req->cgi.len_buf = 0;
    req->timeout = conf->TimeoutCGI;
    req->sock_timer = 0;

    return 0;
}
//======================================================================
void scgi_set_poll_list(Connect *r, int *nsock)
{
    if (r->cgi.status.scgi == SCGI_PARAMS)
    {
        if (r->cgi.len_buf == 0)
        {
            int ret = scgi_set_param(r);
            if (ret < 0)
            {
                fprintf(stderr, "<%s:%d> Error scgi_set_param()\n", __func__, __LINE__);
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
                return;
            }
            else if (ret == 0)
            {
                if (r->cgi.status.scgi == SCGI_READ_HTTP_HEADERS)
                {
                    cgi_poll_fd[*nsock].fd = r->fcgi.fd;
                    cgi_poll_fd[*nsock].events = POLLRDNORM;
                    cgi_poll_array[*nsock] = r;
                }
                else if (r->cgi.status.scgi == SCGI_STDIN)
                {
                    cgi_poll_fd[*nsock].fd = r->clientSocket;
                    cgi_poll_fd[*nsock].events = POLLRDNORM;
                    cgi_poll_array[*nsock] = r;
                }
            }
            else
            {
                cgi_poll_fd[*nsock].fd = r->fcgi.fd;
                cgi_poll_fd[*nsock].events = POLLWRNORM;
                cgi_poll_array[*nsock] = r;
            }
        }
        else
        {
            cgi_poll_fd[*nsock].fd = r->fcgi.fd;
            cgi_poll_fd[*nsock].events = POLLWRNORM;
            cgi_poll_array[*nsock] = r;
        }
    }
    else if (r->cgi.status.scgi == SCGI_STDIN)
    {
        if (r->cgi.dir == FROM_CLIENT)
        {
            cgi_poll_fd[*nsock].fd = r->clientSocket;
            cgi_poll_fd[*nsock].events = POLLRDNORM;
            cgi_poll_array[*nsock] = r;
        }
        else if (r->cgi.dir == TO_CGI)
        {
            cgi_poll_fd[*nsock].fd = r->fcgi.fd;
            cgi_poll_fd[*nsock].events = POLLWRNORM;
            cgi_poll_array[*nsock] = r;
        }
    }
    else //==================== CGI_STDOUT =============================
    {
        if (r->cgi.status.scgi == SCGI_READ_HTTP_HEADERS)
        {
            cgi_poll_fd[*nsock].fd = r->fcgi.fd;
            cgi_poll_fd[*nsock].events = POLLRDNORM;
            cgi_poll_array[*nsock] = r;
        }
        else if (r->cgi.status.scgi == SCGI_SEND_HTTP_HEADERS)
        {
            cgi_poll_fd[*nsock].fd = r->clientSocket;
            cgi_poll_fd[*nsock].events = POLLWRNORM;
            cgi_poll_array[*nsock] = r;
        }
        else if (r->cgi.status.scgi == SCGI_SEND_ENTITY)
        {
            if (r->cgi.dir == FROM_CGI)
            {
                cgi_poll_fd[*nsock].fd = r->fcgi.fd;
                cgi_poll_fd[*nsock].events = POLLRDNORM;
                cgi_poll_array[*nsock] = r;
            }
            else if (r->cgi.dir == TO_CLIENT)
            {
                cgi_poll_fd[*nsock].fd = r->clientSocket;
                cgi_poll_fd[*nsock].events = POLLWRNORM;
                cgi_poll_array[*nsock] = r;
            }
        }
        else
        {
            print_err(r, "<%s:%d> ??? Error: SCGI_STATUS=%s\n", __func__, __LINE__, get_scgi_status(r->cgi.status.scgi));
            if (r->cgi.status.scgi <= SCGI_READ_HTTP_HEADERS)
                r->err = -RS502;
            else
                r->err = -1;
            cgi_del_from_list(r);
            end_response(r);
            return;
        }
    }

    (*nsock)++;
}
//======================================================================
int scgi_set_param(Connect *r)
{
    r->cgi.len_buf = 0;
    r->cgi.p = r->cgi.buf + 8;

    for ( ; r->fcgi.i_param < r->fcgi.size_par; ++r->fcgi.i_param)
    {
        int len_name = r->fcgi.vPar[r->fcgi.i_param].name.size();
        if (len_name == 0)
        {
            fprintf(stderr, "<%s:%d> Error: len_name=0\n", __func__, __LINE__);
            return -1;
        }

        int len_val = r->fcgi.vPar[r->fcgi.i_param].val.size();
        int len = len_name + len_val + 2;

        if (len > (r->cgi.size_buf - r->cgi.len_buf))
        {
            break;
        }

        memcpy(r->cgi.p, r->fcgi.vPar[r->fcgi.i_param].name.c_str(), len_name);
        r->cgi.p += len_name;
        
        memcpy(r->cgi.p, "\0", 1);
        r->cgi.p += 1;

        if (len_val > 0)
        {
            memcpy(r->cgi.p, r->fcgi.vPar[r->fcgi.i_param].val.c_str(), len_val);
            r->cgi.p += len_val;
        }

        memcpy(r->cgi.p, "\0", 1);
        r->cgi.p += 1;

        r->cgi.len_buf += len;
    }

    if (r->cgi.len_buf > 0)
    {      
        scgi_set_size_data(r);
    }
    else
    {
        r->sock_timer = 0;
        if (r->req_hdrs.reqContentLength > 0)
        {
            r->cgi.len_post = r->req_hdrs.reqContentLength - r->lenTail;
            r->cgi.status.scgi = SCGI_STDIN;
            if (r->lenTail > 0)
            {
                r->cgi.dir = TO_CGI;
                r->timeout = conf->TimeoutCGI;
                r->cgi.p = r->tail;
                r->cgi.len_buf = r->lenTail;
                r->tail = NULL;
                r->lenTail = 0;
            }
            else // [r->lenTail == 0]
            {
                r->cgi.dir = FROM_CLIENT;
                r->timeout = conf->TimeOut;
            }
        }
        else
        {
            r->cgi.status.scgi = SCGI_READ_HTTP_HEADERS;
            r->cgi.dir = FROM_CGI;
            r->tail = NULL;
            r->lenTail = 0;
            r->p_newline = r->cgi.p = r->cgi.buf + 8;
            r->cgi.len_buf = 0;
            r->timeout = conf->TimeoutCGI;
        }
    }

    return r->cgi.len_buf;
}
//======================================================================
void scgi_(Connect* r)
{
    if (r->cgi.status.scgi == SCGI_PARAMS)
    {
        int ret = write_to_fcgi(r);
        if (ret == -WSAENOTCONN)
        {
            return;
        }
        else if (ret < 0)
        {
            r->err = -RS502;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
    else if (r->cgi.status.scgi == SCGI_STDIN)
    {
        int n = cgi_stdin(r);
        if (n < 0)
        {
            print_err(r, "<%s:%d> Error cgi_stdin\n", __func__, __LINE__);
            r->err = -1;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
    else //==================== SCGI_STDOUT=============================
    {
        if (r->cgi.status.scgi == SCGI_READ_HTTP_HEADERS)
        {
            int ret = scgi_read_hdrs(r);
            if (ret == -WSAEWOULDBLOCK)
                r->sock_timer = 0;
            else if (ret < 0)
            {
                fprintf(stderr, "<%s:%d> Error cgi_read_hdrs()=%d\n", __func__, __LINE__, ret);
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
            else if (ret > 0)
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
                    r->cgi.status.scgi = SCGI_SEND_HTTP_HEADERS;
                }
            }
            else // ret == 0
                r->sock_timer = 0;
        }
        else if (r->cgi.status.scgi == SCGI_SEND_HTTP_HEADERS)
        {
            if (r->resp_headers.len > 0)
            {
                int wr = send(r->clientSocket, r->resp_headers.p, r->resp_headers.len, 0);
                if (wr < 0)
                {
                    if (errno == EAGAIN)
                        r->sock_timer = 0;
                    else
                    {
                        r->err = -1;
                        r->req_hdrs.iReferer = MAX_HEADERS - 1;
                        r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                        cgi_del_from_list(r);
                        end_response(r);
                    }
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
                            r->cgi.status.scgi = SCGI_SEND_ENTITY;
                            r->sock_timer = 0;
                            if (r->lenTail > 0)
                            {
                                if (r->mode_send == CHUNK)
                                {
                                    memmove(r->cgi.buf + 8, r->tail, r->lenTail);
                                    r->cgi.len_buf = r->lenTail;
                                    r->tail = NULL;
                                    r->lenTail = 0;
                                    if (cgi_set_size_chunk(r))
                                    {
                                        r->err = -1;
                                        cgi_del_from_list(r);
                                        end_response(r);
                                        return;
                                    }
                                }
                                else
                                {
                                    r->cgi.p = r->tail;
                                    r->cgi.len_buf = r->lenTail;
                                    r->tail = NULL;
                                    r->lenTail = 0;
                                }
                                r->cgi.dir = TO_CLIENT;
                                r->timeout = conf->TimeOut;
                            }
                            else
                            {
                                r->cgi.len_buf = 0;
                                r->cgi.p = NULL;
                                r->cgi.dir = FROM_CGI;
                                r->timeout = conf->TimeoutCGI;
                            }
                        }
                    }
                    else
                        r->sock_timer = 0;
                }
            }
            else
            {
                print_err(r, "<%s:%d> Error resp.len=%d\n", __func__, __LINE__, r->resp_headers.len);
                r->err = -1;
                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Error send response headers";
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else if (r->cgi.status.scgi == SCGI_SEND_ENTITY)
        {
            int ret = scgi_stdout(r);
            if (ret == -EAGAIN)
            {
                r->sock_timer = 0;
            }
            else if (ret < 0)
            {
                r->err = -1;
                cgi_del_from_list(r);
                end_response(r);
            }
            else if (ret == 0) // end SCGI_SEND_ENTITY
            {
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else
        {
            print_err(r, "<%s:%d> ??? Error: SCGI_STATUS=%s\n", __func__, __LINE__, get_scgi_status(r->cgi.status.scgi));
            if (r->cgi.status.scgi <= SCGI_READ_HTTP_HEADERS)
                r->err = -RS502;
            else
                r->err = -1;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
    
}
//======================================================================
int scgi_read_hdrs(Connect *r)
{
    unsigned int num_read;
    num_read = r->cgi.size_buf - r->cgi.len_buf - 1;

    if (num_read <= 0)
    {
        r->err = -RS502;
        return -1;
    }

    int n = recv(r->fcgi.fd, r->cgi.p, num_read, 0);
    if (n == SOCKET_ERROR)
    {
        int err = GetLastError();
        if (err == WSAEWOULDBLOCK)
            return -WSAEWOULDBLOCK;
        r->err = -RS502;
        return -1;
    }
    else if (n == 0)
    {
        print_err(r, "<%s:%d> Error: Connection reset by peer\n", __func__, __LINE__);
        r->err = -RS502;
        return -1;
    }

    r->lenTail += n;
    r->cgi.len_buf += n;
    r->cgi.p += n;
    *(r->cgi.p) = 0;
  
    n = cgi_find_empty_line(r);
    if (n == 1) // empty line found
    {
        r->cgi.status.scgi = SCGI_SEND_HTTP_HEADERS;
        r->timeout = conf->TimeOut;
        //r->fcgi.http_headers_received = true;
        r->sock_timer = 0;
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
int scgi_stdout(Connect *req)
{
    if (req->cgi.dir == FROM_CGI)
    {
        int fd = req->fcgi.fd;
        req->cgi.len_buf = recv(fd, req->cgi.buf + 8, req->cgi.size_buf, 0);
        if (req->cgi.len_buf == -1)
        {
            print_err(req, "<%s:%d> Error read from script(): %s\n", __func__, __LINE__, strerror(errno));
            if (errno == EAGAIN)
                return -EAGAIN;
            return -1;
        }
        else if (req->cgi.len_buf == 0)
        {
            if (req->mode_send == CHUNK)
            {
                req->cgi.len_buf = 0;
                cgi_set_size_chunk(req);
                req->cgi.dir = TO_CLIENT;
                req->mode_send = CHUNK_END;
                return 4;
            }

            return 0;
        }

        req->cgi.dir = TO_CLIENT;
        if (req->mode_send == CHUNK)
        {
            if (cgi_set_size_chunk(req))
                return -1;
        }
        else
            req->cgi.p = req->cgi.buf + 8;
        return req->cgi.len_buf;
    }
    else if (req->cgi.dir == TO_CLIENT)
    {
        int ret = send(req->clientSocket, req->cgi.p, req->cgi.len_buf, 0);
        if (ret == -1)
        {
            print_err(req, "<%s:%d> Error send to client: %s\n", __func__, __LINE__, strerror(errno));
            if (errno == EAGAIN)
                return -EAGAIN;
            return -1;
        }

        req->cgi.p += ret;
        req->cgi.len_buf -= ret;
        req->resp.send_bytes += ret;
        if (req->cgi.len_buf == 0)
        {
            req->cgi.dir = FROM_CGI;
        }
        return ret;
    }

    return 0;
}
//======================================================================
int timeout_scgi(Connect *r)
{
    if ((r->cgi.status.scgi == SCGI_STDIN) && (r->cgi.dir == TO_CGI))
        return -RS504;
    else if (r->cgi.status.scgi == SCGI_READ_HTTP_HEADERS)
        return -RS504;
    else
        return -1;
}
