#include "main.h"

using namespace std;
//======================================================================
static Connect *work_list_start = NULL;
static Connect *work_list_end = NULL;

static Connect *wait_list_start = NULL;
static Connect *wait_list_end = NULL;

static fd_set cgi_wrfds, cgi_rdfds;

static mutex mtx_;
static condition_variable cond_;

static int close_thr = 0;
static unsigned int num_wait, num_work;
static int n_work, n_select, n_wait_pipe;

const DWORD PIPE_BUFSIZE = 1024;
const int TimeoutPipe = 10;

static int cgi_stdin(Connect *req);
static int cgi_stdout(Connect *req);
static int cgi_read_hdrs(Connect *req);
static int ReadPipe(Connect *r, char* buf, int sizeBuf, DWORD *nread, int maxRd, int timeout);
static int WritePipe(Connect *r, const char* buf, int lenBuf, int sizePipeBuf, int timeout);
static void timeout_pipe(Connect *r);
static void cgi_set_status_readheaders(Connect *r);
static void cgi_set_status_sendheaders(Connect *r);
static int cgi_create_proc(Connect* req);
static int timeout_cgi(Connect *r);
static int get_resp_status(Connect *r);
static void cgi_worker(Connect* r);

void cgi_set_direct(Connect *r, DIRECT dir);
int cgi_set_size_chunk(Connect *r);
int cgi_find_empty_line(Connect *r);

int fcgi_create_connect(Connect *req);
void fcgi_worker(Connect* r);
int timeout_fcgi(Connect *r);

int scgi_create_connect(Connect *r);
void scgi_worker(Connect* r);
int timeout_scgi(Connect *r);
//======================================================================
wstring cgi_script_file(const wstring& name)
{
    const wchar_t* p;

    if ((p = wcschr(name.c_str() + 1, '/')))
        return p;
    return name;
}
//======================================================================
Cgi::Cgi()
{
    Pipe.parentPipe = INVALID_HANDLE_VALUE;
    lenEnv = 0;
    sizeBufEnv = 0;
    bufEnv = NULL;
}
//======================================================================
Cgi::~Cgi()
{
    if (bufEnv)
        delete[] bufEnv;
}
//======================================================================
int Cgi::init(size_t n)
{
    lenEnv = 0;
    Pipe.parentPipe = INVALID_HANDLE_VALUE;
    if (n > sizeBufEnv)
    {
        sizeBufEnv = n;
        if (bufEnv)
        {
            delete[] bufEnv;
        }

        bufEnv = new(nothrow) char[sizeBufEnv];
        if (!bufEnv)
        {
            sizeBufEnv = 0;
            return -1;
        }
    }

    return 0;
}
//======================================================================
size_t Cgi::param(const char* name, const char* val)
{
    unsigned int lenName, lenVal;
    if (name)
        lenName = strlen(name);
    else
        return 0;
    if (val)
        lenVal = strlen(val);
    else
        lenVal = 0;

    String ss(lenName + lenVal + 2);
    ss << name << '=' << val;

    size_t len = ss.size();
    if ((lenEnv + len + 2) > sizeBufEnv)
    {
        int sizeTmp = sizeBufEnv + len + 128;
        char* tmp = new(nothrow) char[sizeTmp];
        if (!tmp)
            return 0;
        sizeBufEnv = sizeTmp;
        memcpy(tmp, bufEnv, lenEnv);
        delete[] bufEnv;
        bufEnv = tmp;
        return 0;
    }
    memcpy(bufEnv + lenEnv, ss.c_str(), len);
    lenEnv += len;
    bufEnv[lenEnv++] = '\0';
    bufEnv[lenEnv] = '\0';
    return len;
}
//======================================================================
void cgi_del_from_list(Connect *r)
{
    if ((r->scriptType == CGI) || (r->scriptType == PHPCGI))
    {
        if (r->cgi.Pipe.parentPipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
            CloseHandle(r->cgi.Pipe.parentPipe);
        }

        if (r->cgi.hChld != INVALID_HANDLE_VALUE)
        {
            DWORD dwWait = WaitForSingleObject(r->cgi.hChld, 2);
            switch (dwWait)
            {
                case WAIT_OBJECT_0:
                    break;
                case WAIT_TIMEOUT:
                    if (!TerminateProcess(r->cgi.hChld, 0))
                    {
                        DWORD err = GetLastError();
                        if (err != ERROR_ACCESS_DENIED)
                            PrintError(__func__, __LINE__, "TerminateProcess", err);
                    }
                    break;
                case WAIT_FAILED:
                    print_err(r, "<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    break;
                default:
                    print_err(r, "<%s:%d> default\n", __func__, __LINE__);
                    break;
            }
            if (!CloseHandle(r->cgi.hChld))
            {
                PrintError(__func__, __LINE__, "CloseHandle", GetLastError());
            }
        }
    }
    else
    {
        if (r->fcgi.fd != INVALID_SOCKET)
        {
            shutdown(r->fcgi.fd, SD_BOTH);
            closesocket(r->fcgi.fd);
        }
    }

    r->wScriptName = L"";
mtx_.lock();
    --num_work;
mtx_.unlock();
    if (r->prev && r->next)
    {
        r->prev->next = r->next;
        r->next->prev = r->prev;
    }
    else if (r->prev && !r->next)
    {
        r->prev->next = r->next;
        work_list_end = r->prev;
    }
    else if (!r->prev && r->next)
    {
        r->next->prev = r->prev;
        work_list_start = r->next;
    }
    else if (!r->prev && !r->next)
        work_list_start = work_list_end = NULL;
}
//======================================================================
static void cgi_add_work_list()
{
mtx_.lock();
    if ((num_work < conf->MaxCgiProc) && wait_list_end)
    {
        int n_max = conf->MaxCgiProc - num_work;
        Connect *r = wait_list_end;

        for ( ; (n_max > 0) && r; r = wait_list_end, --n_max)
        {
            wait_list_end = r->prev;
            if (wait_list_end == NULL)
                wait_list_start = NULL;
            --num_wait;
            //---------------------------
            if ((r->scriptType == CGI) || (r->scriptType == PHPCGI))
            {
                r->cgi.Pipe.parentPipe = INVALID_HANDLE_VALUE;
                r->cgi.status.cgi = CGI_CREATE_PROC;
            }
            else if ((r->scriptType == PHPFPM) || (r->scriptType == FASTCGI))
            {
                r->io_direct = TO_CGI;
                r->cgi.status.fcgi = FASTCGI_CONNECT;
                r->fcgi.fd = INVALID_SOCKET;
                int ret = fcgi_create_connect(r);
                if (ret < 0)
                {
                    r->err = ret;
                    end_response(r);
                    continue;
                }
            }
            else if (r->scriptType == SCGI)
            {
                r->io_direct = TO_CGI;
                r->cgi.status.scgi = SCGI_CONNECT;
                r->fcgi.fd = INVALID_SOCKET;
                int ret = scgi_create_connect(r);
                if (ret < 0)
                {
                    r->err = ret;
                    end_response(r);
                    continue;
                }
            }
            else
            {
                print_err(r, "<%s:%d> cgi_type=%s\n", __func__, __LINE__, get_cgi_type(r->scriptType));
                end_response(r);
                continue;
            }
            //--------------------------
            if (work_list_end)
                work_list_end->next = r;
            else
                work_list_start = r;

            r->prev = work_list_end;
            r->next = NULL;
            work_list_end = r;
            ++num_work;
        }
    }
mtx_.unlock();
}
//======================================================================
static void cgi_set_select_list(Connect *r)
{
    if (r->io_direct == FROM_CLIENT)
    {
        r->timeout = conf->TimeOut;
        FD_SET(r->clientSocket, &cgi_rdfds);
        n_select++;
    }
    else if (r->io_direct == TO_CLIENT)
    {
        r->timeout = conf->TimeOut;
        FD_SET(r->clientSocket, &cgi_wrfds);
        n_select++;
    }
    else if (r->io_direct == FROM_CGI)
    {
        r->timeout = conf->TimeoutCGI;
        if ((r->scriptType != CGI) && (r->scriptType != PHPCGI))
        {
            FD_SET(r->fcgi.fd, &cgi_rdfds);
            ++n_select;
        }
    }
    else if (r->io_direct == TO_CGI)
    {
        r->timeout = conf->TimeoutCGI;
        if ((r->scriptType != CGI) && (r->scriptType != PHPCGI))
        {
            FD_SET(r->fcgi.fd, &cgi_wrfds);
            ++n_select;
        }
    }
}
//======================================================================
static void cgi_set_select_list()
{
    __time64_t t = _time64(NULL);
    n_select = n_wait_pipe = n_work = 0;
    Connect *r = work_list_start, *next = NULL;
    for ( ; r; r = next)
    {
        next = r->next;

        if (r->sock_timer == 0)
            r->sock_timer = t;

        if ((t - r->sock_timer) >= r->timeout)
        {
            print_err(r, "<%s:%d> Timeout=%llu, dir=%s, type=%s\n", __func__, __LINE__,
                    t - r->sock_timer, get_cgi_dir(r->io_direct), get_cgi_type(r->scriptType));
            if ((r->scriptType == CGI) || (r->scriptType == PHPCGI))
            {
                r->err = timeout_cgi(r);
            }
            else if ((r->scriptType == PHPFPM) || (r->scriptType == FASTCGI))
            {
                r->err = timeout_fcgi(r);
            }
            else if (r->scriptType == SCGI)
            {
                r->err = timeout_scgi(r);
            }
            else
                r->err = -1;
            r->req_hdrs.iReferer = MAX_HEADERS - 1;
            r->req_hdrs.Value[r->req_hdrs.iReferer] = "Timeout";
            cgi_del_from_list(r);
            end_response(r);
        }
        else
        {
            if (r->io_status == WORK)
                ++n_work;
            else if (r->io_status == WAIT_PIPE)
                ++n_wait_pipe;
            else
                cgi_set_select_list(r);
        }
    }
}
//======================================================================
void select_worker(Connect *r)
{
    if ((r->scriptType == CGI) || (r->scriptType == PHPCGI))
        cgi_worker(r);
    else if ((r->scriptType == PHPFPM) || (r->scriptType == FASTCGI))
        fcgi_worker(r);
    else if (r->scriptType == SCGI)
        scgi_worker(r);
}
//======================================================================
static int cgi_poll(int num_chld)
{
    int ret = 0;
    if (n_select > 0)
    {
        int time_sel = conf->TimeoutSel*1000;
        if ((n_wait_pipe + n_work) > 0)
            time_sel = 0;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = time_sel;

        ret = select(0, &cgi_rdfds, &cgi_wrfds, NULL, &tv);
        if (ret == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error select()", WSAGetLastError());
            return -1;
        }
        else if (ret == 0)
        {
            if ((n_wait_pipe + n_work) == 0)
                return 0;
        }
    }
    else
    {
        if ((n_wait_pipe + n_work) == 0)
            return 0;
    }

    int all = ret + n_wait_pipe + n_work;
    Connect *r = work_list_start, *next = NULL;
    for ( ; (all > 0) && r; r = next)
    {
        next = r->next;

        if (r->io_status == WAIT_PIPE)
        {
            --all;
            timeout_pipe(r);
        }
        else if (r->io_status == WORK)
        {
            --all;
            select_worker(r);
        }
        else if (r->io_status == SELECT)
        {
            if (FD_ISSET(r->clientSocket, &cgi_rdfds))
            {
                --all;
                r->io_status = WORK;
                select_worker(r);
            }
            else if (FD_ISSET(r->clientSocket, &cgi_wrfds))
            {
                --all;
                r->io_status = WORK;
                select_worker(r);
            }
            else if (FD_ISSET(r->fcgi.fd, &cgi_rdfds))
            {
                --all;
                r->io_status = WORK;
                select_worker(r);
            }
            else if (FD_ISSET(r->fcgi.fd, &cgi_wrfds))
            {
                --all;
                r->io_status = WORK;
                select_worker(r);
            }
        }
    }

    return 0;
}
//======================================================================
void cgi_handler(int num_chld)
{
    while (1)
    {
        {
    unique_lock<mutex> lk(mtx_);
            while ((!work_list_start) && (!wait_list_start) && (!close_thr))
            {
                cond_.wait(lk);
            }

            if (close_thr)
                break;
        }

        FD_ZERO(&cgi_rdfds);
        FD_ZERO(&cgi_wrfds);
        cgi_add_work_list();
        cgi_set_select_list();
        if (cgi_poll(num_chld) < 0)
            break;
    }
}
//======================================================================
void push_cgi(Connect *r)
{
    r->io_status = WORK;
    r->operation = DYN_PAGE;
    r->resp.respStatus = RS200;
    r->sock_timer = 0;
    r->prev = NULL;
mtx_.lock();
    r->next = wait_list_start;
    if (wait_list_start)
        wait_list_start->prev = r;
    wait_list_start = r;
    if (!wait_list_end)
        wait_list_end = r;
    ++num_wait;
mtx_.unlock();
    cond_.notify_one();
}
//======================================================================
void close_cgi_handler(void)
{
    close_thr = 1;
    cond_.notify_one();
}
//======================================================================
int cgi_stdin(Connect *r)
{
    if (r->io_direct == FROM_CLIENT)
    {
        int rd = (r->cgi.len_post > r->cgi.size_buf) ? r->cgi.size_buf : r->cgi.len_post;
        r->cgi.len_buf = recv(r->clientSocket, r->cgi.buf, rd, 0);
        if (r->cgi.len_buf == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                r->io_status = SELECT;
                return TRYAGAIN;
            }
            return -RS502;
        }
        else if (r->cgi.len_buf == 0)
        {
            print_err(r, "<%s:%d> Error recv()=0\n", __func__, __LINE__);
            return -1;
        }

        r->cgi.len_post -= r->cgi.len_buf;
        r->cgi.p = r->cgi.buf;
        cgi_set_direct(r, TO_CGI);
    }
    else if (r->io_direct == TO_CGI)
    {
        // write to pipe
        int ret = WritePipe(r, r->cgi.p, r->cgi.len_buf, PIPE_BUFSIZE, TimeoutPipe);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
                return TRYAGAIN;
            print_err(r, "<%s:%d> Error write_to_script()=%d\n", __func__, __LINE__, ret);
            return -RS502;
        }

        r->cgi.p += ret;
        r->cgi.len_buf -= ret;
        if (r->cgi.len_buf == 0)
        {
            if (r->cgi.len_post == 0)
            {
                cgi_set_status_readheaders(r);
            }
            else
            {
                cgi_set_direct(r, FROM_CLIENT);
            }
        }
    }

    return 0;
}
//======================================================================
int cgi_stdout(Connect *r)// return [ TRYAGAIN | -1 | 0 | 1 | 0< ]
{
    if (r->io_direct == FROM_CGI)
    {
        // read from pipe
        DWORD read_bytes = 0;
        int ret = ReadPipe(r, r->cgi.buf + 8, r->cgi.size_buf, &read_bytes, PIPE_BUFSIZE, TimeoutPipe);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
                return TRYAGAIN;
            print_err(r, "<%s:%d> ! Error ReadPipe()=%d, read_bytes=%d\n", __func__, __LINE__, ret, read_bytes);
            return get_resp_status(r);
        }
        else if (ret == 0)
        {
            if (r->mode_send == CHUNK)
            {
                r->cgi.len_buf = 0;
                r->cgi.p = r->cgi.buf + 8;
                cgi_set_size_chunk(r);
                cgi_set_direct(r, TO_CLIENT);
                r->mode_send = CHUNK_END;
                return r->cgi.len_buf;
            }
            return 0;
        }

        r->cgi.len_buf = read_bytes;
        cgi_set_direct(r, TO_CLIENT);
        r->cgi.p = r->cgi.buf + 8;
        if (r->mode_send == CHUNK)
        {
            if (cgi_set_size_chunk(r))
                return -1;
        }

        return r->cgi.len_buf;
    }
    else if (r->io_direct == TO_CLIENT)
    {
        int ret = send(r->clientSocket, r->cgi.p, r->cgi.len_buf, 0);
        if (ret == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                r->io_status = SELECT;
                return TRYAGAIN;
            }
            return -1;
        }

        r->cgi.p += ret;
        r->cgi.len_buf -= ret;
        r->resp.send_bytes += ret;
        if (r->cgi.len_buf == 0)
        {
            if (r->mode_send == CHUNK_END)
                return 0;
            else
            {
                cgi_set_direct(r, FROM_CGI);
            }
        }
        r->sock_timer = 0;
        return ret;
    }

    return -1;
}
//======================================================================
static void cgi_worker(Connect* r)
{
    if (r->cgi.status.cgi == CGI_CREATE_PROC)
    {
        int ret = cgi_create_proc(r);
        if (ret < 0)
        {
            print_err(r, "<%s:%d> Error cgi_create_proc()=%d\n", __func__, __LINE__, ret);
            r->err = ret;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
    else if (r->cgi.status.cgi == CGI_STDIN)
    {
        int ret = cgi_stdin(r);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
            {
                //print_err(r, "<%s:%d> ---TRYAGAIN---\n", __func__, __LINE__);
            }
            else
            {
                print_err(r, "<%s:%d> Error cgi_stdin\n", __func__, __LINE__);
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else
            r->sock_timer = 0;
    }
    else if (r->cgi.status.cgi == CGI_READ_HTTP_HEADERS)
    {
        int rd = cgi_read_hdrs(r);
        if (rd < 0)
        {
            if (rd == TRYAGAIN)
            {
                //print_err(r, "<%s:%d> ---TRYAGAIN---\n", __func__, __LINE__);
            }
            else
            {
                r->err = rd;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else if (rd > 0)
        {
            if (create_response_headers(r))
            {
                print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
                r->err = -1;
                cgi_del_from_list(r);
                end_response(r);
                return;
            }
            else
            {
                cgi_set_status_sendheaders(r);
            }
        }
        else
            r->sock_timer = 0;
    }
    else if (r->cgi.status.cgi == CGI_SEND_HTTP_HEADERS)
    {
        if (r->resp_headers.len > 0)
        {
            int wr = send(r->clientSocket, r->resp_headers.p, r->resp_headers.len, 0);
            if (wr == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                    r->io_status = SELECT;
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
                        r->cgi.status.cgi = CGI_SEND_ENTITY;
                        if (r->lenTail > 0)
                        {
                            r->sock_timer = 0;
                            if (r->mode_send == CHUNK)
                            {
                                r->cgi.len_buf = r->lenTail;
                                r->cgi.p = r->tail;
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
                                r->lenTail = 0;
                            }

                            cgi_set_direct(r, TO_CLIENT);
                        }
                        else
                        {
                            r->cgi.len_buf = 0;
                            r->cgi.p = NULL;
                            cgi_set_direct(r, FROM_CGI);
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
    else if (r->cgi.status.cgi == CGI_SEND_ENTITY)
    {
        int ret = cgi_stdout(r);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
            {
                //print_err(r, "<%s:%d> ---TRYAGAIN---\n", __func__, __LINE__);
            }
            else
            {
                r->err = -1;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else if (ret == 0)// end
        {
            cgi_del_from_list(r);
            end_response(r);
        }
        else
        {
            r->sock_timer = 0;
        }
    }
    else
    {
        print_err(r, "<%s:%d> ??? Error status=%d\n", __func__, __LINE__, r->cgi.status.cgi);
        r->err = -1;
        cgi_del_from_list(r);
        end_response(r);
    }
}
//======================================================================
int cgi_find_empty_line(Connect *r)
{
    char *pCR, *pLF;
    while (r->lenTail > 0)
    {
        int i = 0, len_line = 0;
        pCR = pLF = NULL;
        while (i < r->lenTail)
        {
            char ch = *(r->p_newline + i);
            if (ch == '\r')// found CR
            {
                if (i == (r->lenTail - 1))
                    return 0;
                if (pCR)
                {
                    print_err(r, "<%s:%d> Error (pCR)\n", __func__, __LINE__);
                    return -RS502;
                }
                pCR = r->p_newline + i;
            }
            else if (ch == '\n')// found LF
            {
                pLF = r->p_newline + i;
                if ((pCR) && ((pLF - pCR) != 1))
                {
                    return -RS502;
                }
                i++;
                break;
            }
            else
                len_line++;
            i++;
        }

        if (pLF) // found end of line '\n'
        {
            if (pCR == NULL)
                *pLF = 0;
            else
                *pCR = 0;

            if (len_line == 0)
            {
                r->lenTail -= i;
                if (r->lenTail > 0)
                    r->tail = pLF + 1;
                else
                    r->tail = NULL;
                return 1;
            }

            if (!memchr(r->p_newline, ':', len_line))
            {
                print_err(r, "<%s:%d> Error Line not header: [%s]\n", __func__, __LINE__, r->p_newline);
                return -RS502;
            }

            if (!strlcmp_case(r->p_newline, "Status", 6))
            {
                r->resp.respStatus = atoi(r->p_newline + 7);
            }
            else
                r->hdrs << r->p_newline << "\r\n";

            r->lenTail -= i;
            r->p_newline = pLF + 1;
        }
        else if (pCR && (!pLF))
        {
            print_err(r, "<%s:%d> Error (pCR && (!pLF))\n", __func__, __LINE__);
            return -RS502;
        }
        else
            break;
    }

    return 0;
}
//======================================================================
int cgi_read_hdrs(Connect *r)
{
    if ((r->scriptType == CGI) || (r->scriptType == PHPCGI))
    {
        // read_from_pipe;
        unsigned int len_read = r->cgi.size_buf - r->cgi.len_buf - 1;
        if (len_read <= 0)
            return -RS505;
        if (len_read > PIPE_BUFSIZE)
            len_read = PIPE_BUFSIZE;
        DWORD read_bytes;
        int ret = ReadPipe(r, r->cgi.p, len_read, &read_bytes, PIPE_BUFSIZE, TimeoutPipe);
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
            {
                //print_err(r, "<%s:%d> ---TRYAGAIN---\n", __func__, __LINE__);
                return TRYAGAIN;
            }
            return -1;
        }
        else if (ret == 0)
        {
            print_err(r, "<%s:%d> Error ReadPipe=%d\n", __func__, __LINE__, ret);
            return -1;
        }
        else
        {
            r->lenTail += read_bytes;
            r->cgi.len_buf += read_bytes;
            r->cgi.p += read_bytes;
            *(r->cgi.p) = 0;
            ret = cgi_find_empty_line(r);
            if (ret == 1) // empty line found
            {
                return r->cgi.len_buf;
            }
            else if (ret < 0) // error
                return -1;
        }
    }
    else
    {
        print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__, r->scriptType);
        return -1;
    }

    return 0;
}
//======================================================================
int get_overlap_result(Connect *r, DWORD *read_bytes)
{
    *read_bytes = 0;
    bool bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, read_bytes, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            //print_err(r, "<%s:%d> ---ERROR_BROKEN_PIPE--\n", __func__, __LINE__);
            if (*read_bytes > 0)
                print_err(r, "<%s:%d> ??? read_bytes=%u [ERROR_BROKEN_PIPE]\n", __func__, __LINE__, *read_bytes);
            return 0;
        }
        else if (err == ERROR_IO_INCOMPLETE) // 996
        {
            //print_err(r, "<%s:%d> ------ERROR_IO_INCOMPLETE-----\n", __func__, __LINE__);
            r->io_status = WAIT_PIPE;
            return TRYAGAIN;
        }
        return -1;
    }

    r->io_status = WORK;
    return *read_bytes;
}
//======================================================================
void timeout_pipe(Connect *r)
{
    DWORD ready_bytes;
    int ret = get_overlap_result(r, &ready_bytes);
    if (ret < 0)
    {
        if (ret != TRYAGAIN)
        {
            print_err(r, "<%s:%d> Error get_overlap_result()=%d\n", __func__, __LINE__, ret);
            r->err = timeout_cgi(r);
            cgi_del_from_list(r);
            end_response(r);
        }
        return;
    }

    r->io_status = WORK;
    if (r->cgi.status.cgi == CGI_STDIN)
    {
        if (r->io_direct == TO_CGI)
        {
            print_err(r, "<%s:%d> ???\n", __func__, __LINE__);
            if (ret > 0)
            {
                r->cgi.p += ret;
                r->cgi.len_buf -= ret;
                if (r->cgi.len_buf == 0)
                {
                    if (r->cgi.len_post == 0)
                    {
                        cgi_set_status_readheaders(r);
                    }
                    else
                    {
                        cgi_set_direct(r, FROM_CLIENT);
                    }
                }
            }
            else if (ret == 0)
            {
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi.status.cgi == CGI_READ_HTTP_HEADERS)
    {
        if (ret > 0)
        {
            r->lenTail += ready_bytes;
            r->cgi.len_buf += ready_bytes;
            r->cgi.p += ready_bytes;
            *(r->cgi.p) = 0;
            int n = cgi_find_empty_line(r);
            if (n == 1) // empty line found
            {
                if (create_response_headers(r))
                {
                    print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
                else
                {
                    cgi_set_status_sendheaders(r);
                }
            }
            else if (n < 0) // error
            {
                print_err(r, "<%s:%d> Error cgi_find_empty_line()\n", __func__, __LINE__);
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else if (ret == 0)
        {
            print_err(r, "<%s:%d> ??? Error get_overlap_result()=0\n", __func__, __LINE__);
            r->err = -RS502;
            cgi_del_from_list(r);
            end_response(r);
        }
    }
    else if ((r->cgi.status.cgi == CGI_SEND_ENTITY) && (r->io_direct == FROM_CGI))
    {
        if (ret > 0)
        {
            r->cgi.len_buf = ready_bytes;
            r->cgi.p = r->cgi.buf + 8;
            cgi_set_direct(r, TO_CLIENT);
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
        else if (ret == 0)
        {
            if (r->mode_send == CHUNK)
            {
                r->cgi.len_buf = 0;
                r->cgi.p = r->cgi.buf + 8;
                cgi_set_size_chunk(r);
                cgi_set_direct(r, TO_CLIENT);
                r->mode_send = CHUNK_END;
            }
            else
            {
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
}
//======================================================================
void cgi_set_status_readheaders(Connect *r)
{
    r->cgi.status.cgi = CGI_READ_HTTP_HEADERS;
    cgi_set_direct(r, FROM_CGI);
    r->tail = NULL;
    r->lenTail = 0;
    r->p_newline = r->cgi.p = r->cgi.buf + 8;
    r->cgi.len_buf = 0;
    r->cgi.buf[0] = 0;
}
//======================================================================
void cgi_set_status_sendheaders(Connect *r)
{
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();
    r->cgi.status.cgi = CGI_SEND_HTTP_HEADERS;
    cgi_set_direct(r, TO_CLIENT);
}
//======================================================================
int cgi_create_proc(Connect* r)
{
    size_t len;
    struct _stat st;
    BOOL bSuccess;
    String stmp;

    char pipeName[128] = "\\\\.\\pipe\\cgi";

    if (r->cgi.init(1500) < 0)
    {
        print_err(r, "<%s:%d> Error init param object\n", __func__, __LINE__);
        return -RS500;
    }

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE childPipe;
    ZeroMemory(&r->cgi.Pipe.oOverlap, sizeof(r->cgi.Pipe.oOverlap));

    wstring commandLine;
    wstring wPath;

    if (r->scriptType == CGI)
    {
        wPath = conf->wCgiDir + cgi_script_file(r->wScriptName);
    }
    else if (r->scriptType == PHPCGI)
    {
        wPath = conf->wRootDir + r->wScriptName;
    }

    if (_wstat(wPath.c_str(), &st) == -1)
    {
        utf16_to_utf8(wPath, stmp);
        print_err(r, "<%s:%d> script (%s) not found\n", __func__, __LINE__, stmp.c_str());
        return -RS404;
    }
    //--------------------- set environment ----------------------------
    {
        const int size = 4096;
        char tmpBuf[size];
        if (GetWindowsDirectory(tmpBuf, size))
        {
            r->cgi.param("SYSTEMROOT", tmpBuf);
        }
        else
        {
            print_err(r, "<%s:%d> Error GetWindowsDirectory()\n", __func__, __LINE__);
            return -RS500;
        }
    }

    {
        const int size = 4096;
        char tmpBuf[size];
        if (ExpandEnvironmentStringsA("PATH=%PATH%", tmpBuf, size))
        {
            r->cgi.param("PATH", tmpBuf);
        }
        else
        {
            print_err(r, "<%s:%d> Error ExpandEnvironmentStringsA()\n", __func__, __LINE__);
            return -RS500;
        }
    }

    if (r->reqMethod == M_POST)
    {
        if (r->req_hdrs.iReqContentLength >= 0)
            r->cgi.param("CONTENT_LENGTH", r->req_hdrs.Value[r->req_hdrs.iReqContentLength]);
        else
        {
            return -RS411;
        }

        if (r->req_hdrs.reqContentLength > conf->ClientMaxBodySize)
        {
            r->connKeepAlive = 0;
            return -RS413;
        }

        if (r->req_hdrs.iReqContentType >= 0)
            r->cgi.param("CONTENT_TYPE", r->req_hdrs.Value[r->req_hdrs.iReqContentType]);
        else
        {
            print_err(r, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }
    }

    if (r->scriptType == PHPCGI)
        r->cgi.param("REDIRECT_STATUS", "true");
    if (r->req_hdrs.iUserAgent >= 0)
        r->cgi.param("HTTP_USER_AGENT", r->req_hdrs.Value[r->req_hdrs.iUserAgent]);
    if (r->req_hdrs.iReferer >= 0)
        r->cgi.param("HTTP_REFERER", r->req_hdrs.Value[r->req_hdrs.iReferer]);
    if (r->req_hdrs.iHost >= 0)
        r->cgi.param("HTTP_HOST", r->req_hdrs.Value[r->req_hdrs.iHost]);

    r->cgi.param("SERVER_PORT", conf->ServerPort.c_str());

    r->cgi.param("SERVER_SOFTWARE", conf->ServerSoftware.c_str());
    r->cgi.param("GATEWAY_INTERFACE", "CGI/1.1");

    if (r->reqMethod == M_HEAD)
        r->cgi.param("REQUEST_METHOD", get_str_method(M_GET));
    else
        r->cgi.param("REQUEST_METHOD", get_str_method(r->reqMethod));

    r->cgi.param("REMOTE_HOST", r->remoteAddr);
    r->cgi.param("SERVER_PROTOCOL", get_str_http_prot(r->httpProt));

    utf16_to_utf8(conf->wRootDir, stmp);
    r->cgi.param("DOCUMENT_ROOT", stmp.c_str());

    r->cgi.param("REQUEST_URI", r->uri);

    utf16_to_utf8(wPath, stmp);
    r->cgi.param("SCRIPT_FILENAME", stmp.c_str());

    //utf16_to_utf8(req->wDecodeUri, stmp);
    //env.add("SCRIPT_NAME", stmp.c_str());
    r->cgi.param("SCRIPT_NAME", r->decodeUri);

    r->cgi.param("REMOTE_ADDR", r->remoteAddr);
    r->cgi.param("REMOTE_PORT", r->remotePort);
    r->cgi.param("QUERY_STRING", r->sReqParam);
    //------------------------------------------------------------------
    if (r->scriptType == PHPCGI)
    {
        commandLine = conf->wPathPHP_CGI;
    }
    else if (r->scriptType == CGI)
    {
        if (wcsstr(r->wScriptName.c_str(), L".pl") || wcsstr(r->wScriptName.c_str(), L".cgi"))
        {
            commandLine = conf->wPerlPath + L' ' + wPath;
        }
        else if (wcsstr(r->wScriptName.c_str(), L".py"))
        {
            commandLine = conf->wPyPath + L' ' + wPath;
        }
        else
        {
            commandLine = wPath;
        }
    }
    else
    {
        print_err(r, "<%s:%d> Error CommandLine CreateProcess()\n", __func__, __LINE__);
        return -RS500;
    }
    //------------------------------------------------------------------

    len = strlen(pipeName);
    snprintf(pipeName + len, sizeof(pipeName) - len, "%d-%d-%d", r->numChld, r->numConn, r->numReq);
    r->cgi.Pipe.parentPipe = CreateNamedPipeA(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE |
        PIPE_READMODE_BYTE |
        PIPE_WAIT,
        1,
        PIPE_BUFSIZE,
        PIPE_BUFSIZE,
        5000,
        NULL);
    if (r->cgi.Pipe.parentPipe == INVALID_HANDLE_VALUE)
    {
        PrintError(__func__, __LINE__, "CreateNamedPipe", GetLastError());
        return -RS500;
    }

    if (!SetHandleInformation(r->cgi.Pipe.parentPipe, HANDLE_FLAG_INHERIT, 0))
    {
        PrintError(__func__, __LINE__, "SetHandleInformation", GetLastError());
        DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
        CloseHandle(r->cgi.Pipe.parentPipe);
        r->cgi.Pipe.parentPipe = INVALID_HANDLE_VALUE;
        return -RS500;
    }
    //------------------------------------------------------------------
    childPipe = CreateFileA(
                            pipeName,
                            GENERIC_WRITE | GENERIC_READ,
                            0,
                            &saAttr,
                            OPEN_EXISTING,
                            0,
                            NULL);
    if (childPipe == INVALID_HANDLE_VALUE)
    {
        PrintError(__func__, __LINE__, "CreateFile", GetLastError());
        DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
        CloseHandle(r->cgi.Pipe.parentPipe);
        r->cgi.Pipe.parentPipe = INVALID_HANDLE_VALUE;
        return -RS500;
    }

    ConnectNamedPipe(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap);

    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));

    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = childPipe;
    si.hStdInput = childPipe;
    si.hStdError = GetHandleLogErr();
    si.dwFlags |= STARTF_USESTDHANDLES;

    bSuccess = CreateProcessW(NULL, (wchar_t*)commandLine.c_str(), NULL, NULL, TRUE, 0, r->cgi.bufEnv, NULL, &si, &pi);
    CloseHandle(childPipe);
    if (!bSuccess)
    {
        utf16_to_utf8(commandLine, stmp);
        print_err(r, "<%s:%d> Error CreateProcessW(%s)\n", __func__, __LINE__, stmp.c_str());
        PrintError(__func__, __LINE__, "Error CreateProcessW()", GetLastError());
        DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
        CloseHandle(r->cgi.Pipe.parentPipe);
        r->cgi.Pipe.parentPipe = INVALID_HANDLE_VALUE;
        return -RS500;
    }
    else
    {
        //CloseHandle(pi.hProcess);
        r->cgi.hChld = pi.hProcess;
        CloseHandle(pi.hThread);
    }
    //------------------------------------------------------------------
    if (r->reqMethod == M_POST)
    {
        if (r->req_hdrs.reqContentLength > 0)
        {
            r->cgi.len_post = r->req_hdrs.reqContentLength - r->lenTail;
            r->cgi.status.cgi = CGI_STDIN;
            if (r->lenTail > 0)
            {
                cgi_set_direct(r, TO_CGI);
                r->cgi.p = r->tail;
                r->cgi.len_buf = r->lenTail;
                r->tail = NULL;
                r->lenTail = 0;
            }
            else // [r->lenTail == 0]
                cgi_set_direct(r, FROM_CLIENT);
        }
        else //  (r->req_hdrs.reqContentLength == 0)
            cgi_set_status_readheaders(r);
    }
    else
    {
        cgi_set_status_readheaders(r);
    }

    r->tail = NULL;
    r->lenTail = 0;
    r->sock_timer = 0;

    r->mode_send = ((r->httpProt == HTTP11) && r->connKeepAlive) ? CHUNK : NO_CHUNK;

    return 0;
}
//======================================================================
int cgi_set_size_chunk(Connect *r)
{
    int size = r->cgi.len_buf;
    const char *hex = "0123456789ABCDEF";
    memcpy(r->cgi.p + r->cgi.len_buf, "\r\n", 2);
    int i = 7;
    *(--r->cgi.p) = '\n';
    *(--r->cgi.p) = '\r';
    i -= 2;
    for ( ; i >= 0; --i)
    {
        *(--r->cgi.p) = hex[size % 16];
        size /= 16;
        if (size == 0)
            break;
    }

    if (size != 0)
        return -1;
    r->cgi.len_buf += (8 - i + 2);

    return 0;
}
//======================================================================
int ReadPipe(Connect *r, char* buf, int sizeBuf, DWORD *read_bytes, int maxRd, int timeout)
{
    DWORD rd = (sizeBuf < maxRd) ? sizeBuf : maxRd;
    bool bSuccess = ReadFile(r->cgi.Pipe.parentPipe, buf, rd, NULL, &r->cgi.Pipe.oOverlap);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE)     // 109
        {
            //print_err(r, "<%s:%d> ReadFile: ERROR_BROKEN_PIPE\n", __func__, __LINE__);
            return 0;
        }
        else if (err == ERROR_IO_PENDING) // 997
        {
            //print_err(r, "<%s:%d> WAIT_TIMEOUT\n", __func__, __LINE__);
            r->io_status = WAIT_PIPE;
            return TRYAGAIN;
        }
        else
            return -1;
    }

    bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, read_bytes, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            print_err(r, "<%s:%d> GetOverlappedResult: ERROR_BROKEN_PIPE\n", __func__, __LINE__);
            if (*read_bytes > 0)
                print_err(r, "<%s:%d> ??? read_bytes=%u [ERROR_BROKEN_PIPE]\n", __func__, __LINE__, *read_bytes);
            return 0;
        }
        else if (err == ERROR_IO_INCOMPLETE)
        {
            //print_err(r, "<%s:%d>--- ERROR_IO_INCOMPLETE ---\n", __func__, __LINE__);
            r->io_status = WAIT_PIPE;
            return TRYAGAIN;
        }
        else
        {
            PrintError(__func__, __LINE__, "GetOverlappedResult", err);
            return -1;
        }
    }
    return 1;
}
//======================================================================
int WritePipe(Connect *r, const char* buf, int lenBuf, int sizePipeBuf, int timeout)
{
    DWORD dwWrite = -1;
    bool bSuccess = false;

    int wr = (lenBuf > sizePipeBuf) ? sizePipeBuf : lenBuf;
    bSuccess = WriteFile(r->cgi.Pipe.parentPipe, buf, wr, NULL, &r->cgi.Pipe.oOverlap);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)  //  997
        {
            print_err(r, "<%s:%d> WAIT_TIMEOUT\n", __func__, __LINE__);
            r->io_status = WAIT_PIPE;
            return TRYAGAIN;
        }
        else
            return -1;
    }

    bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, &dwWrite, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE)
        {
            print_err(r, "<%s:%d>--- ERROR_IO_INCOMPLETE ---\n", __func__, __LINE__);
            r->io_status = WAIT_PIPE;
            return TRYAGAIN;
        }
        else
        {
            PrintError(__func__, __LINE__, "GetOverlappedResult", err);
            return -1;
        }
    }

    return dwWrite;
}
//======================================================================
int timeout_cgi(Connect *r)
{
    if ((r->cgi.status.cgi <= CGI_READ_HTTP_HEADERS) &&
       ((r->io_direct == TO_CGI) || (r->io_direct == FROM_CGI)))
        return -RS504;
    else
        return -1;
}
//======================================================================
int get_resp_status(Connect *r)
{
    if ((r->cgi.status.cgi <= CGI_READ_HTTP_HEADERS) &&
       ((r->io_direct == TO_CGI) || (r->io_direct == FROM_CGI)))
        return -RS502;
    else
        return -1;
}
//======================================================================
void cgi_set_direct(Connect *r, DIRECT dir)
{
    r->io_status = WORK;
    r->sock_timer = 0;

    switch (dir)
    {
        case FROM_CGI:
            r->io_direct = FROM_CGI;
            r->timeout = conf->TimeoutCGI;
            break;
        case TO_CGI:
            r->io_direct = TO_CGI;
            r->timeout = conf->TimeoutCGI;
            break;
        case TO_CLIENT:
            r->io_direct = TO_CLIENT;
            r->timeout = conf->TimeOut;
            break;
        case FROM_CLIENT:
            r->io_direct = FROM_CLIENT;
            r->timeout = conf->TimeOut;
            break;
    }
}
