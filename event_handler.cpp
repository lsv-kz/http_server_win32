#include "main.h"

using namespace std;
//======================================================================
static Connect* work_list_start = NULL;
static Connect* work_list_end = NULL;

static Connect* wait_list_start = NULL;
static Connect* wait_list_end = NULL;

static fd_set wrfds, rdfds;

static mutex mtx_;
static condition_variable cond_;

static int close_thr = 0;
static unsigned int n_select, n_work;
static int size_buf;
static char* snd_buf;

static int send_html(Connect* r);
static void set_part(Connect *r);
static void worker(Connect* r, RequestManager* ReqMan);

int create_multipart_head(Connect *req);
//======================================================================
static int send_entity(Connect* req)
{
    int ret;
    int len;

    if (req->resp.respContentLength >= (long long)size_buf)
        len = size_buf;
    else
    {
        len = (int)req->resp.respContentLength;
        if (len == 0)
            return 0;
    }

    ret = send_file(req->clientSocket, req->resp.fd, snd_buf, len);
    if (ret < 0)
    {
        if (ret == -1)
            print_err(req, "<%s:%d> Error: Sent %lld bytes\n", __func__, __LINE__, req->resp.send_bytes);
        return ret;
    }

    req->resp.send_bytes += ret;
    req->resp.respContentLength -= ret;
    if (req->resp.respContentLength == 0)
        ret = 0;

    return ret;
}
//======================================================================
static void del_from_list(Connect *r)
{
    if ((r->source_entity == FROM_FILE) || (r->source_entity == MULTIPART_ENTITY))
        _close(r->resp.fd);

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
static int set_list()
{
mtx_.lock();
    if (wait_list_start)
    {
        if (work_list_end)
            work_list_end->next = wait_list_start;
        else
            work_list_start = wait_list_start;

        wait_list_start->prev = work_list_end;
        work_list_end = wait_list_end;
        wait_list_start = wait_list_end = NULL;
    }
mtx_.unlock();

    __time64_t t = _time64(NULL);
    n_work = n_select = 0;
    Connect* r = work_list_start, *next;

    for (; r; r = next)
    {
        next = r->next;

        if (r->sock_timer == 0)
            r->sock_timer = t;

        if ((int)(t - r->sock_timer) >= r->timeout)
        {
            print_err(r, "<%s:%d> Timeout = %ld\n", __func__, __LINE__, (long)(t - r->sock_timer));
            if (r->operation != READ_REQUEST)
            {
                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Timeout";
            }
            r->err = -1;
            del_from_list(r);
            end_response(r);
        }
        else
        {
            if (r->io_status == WORK)
            {
                ++n_work;
            }
            else
            {
                if (r->io_direct == FROM_CLIENT)
                {
                    r->timeout = conf->TimeOut;
                    FD_SET(r->clientSocket, &rdfds);
                    ++n_select;
                }
                else if (r->io_direct == TO_CLIENT)
                {
                    r->timeout = conf->TimeOut;
                    FD_SET(r->clientSocket, &wrfds);
                    ++n_select;
                }
                else
                {
                    print_err(r, "<%s:%d> Error: io_direct=%d, operation=%d\n", __func__, __LINE__,
                                r->io_direct, r->operation);
                    r->err = -1;
                    del_from_list(r);
                    end_response(r);
                }
            }
        }
    }

    return 1;
}
//======================================================================
static int select_(int num_chld, RequestManager* ReqMan)
{
    int ret = 0;
    if (n_select > 0)
    {
        int time_sel = conf->TimeoutSel*1000;
        if (n_work > 0)
            time_sel = 0;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = time_sel;

        ret = select(0, &rdfds, &wrfds, NULL, &tv);
        if (ret == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error select()", WSAGetLastError());
            return -1;
        }
        else if (ret == 0)
        {
            if (n_work == 0)
                return 0;
        }
    }
    else
    {
        if (n_work == 0)
            return 0;
    }

    int all = ret + n_work;
    Connect* r = work_list_start, *next = NULL;
    for ( ; (all > 0) && r; r = next)
    {
        next = r->next;

        if (r->io_status == WORK)
        {
            --all;
            worker(r, ReqMan);
            continue;
        }

        if (FD_ISSET(r->clientSocket, &rdfds))
        {
            --all;
            r->io_status = WORK;
            worker(r, ReqMan);
        }
        else if (FD_ISSET(r->clientSocket, &wrfds))
        {
            --all;
            r->io_status = WORK;
            worker(r, ReqMan);
        }
    }

    return 1;
}
//======================================================================
void event_handler(RequestManager* ReqMan)
{
    int num_chld = ReqMan->get_num_chld();
    size_buf = conf->SndBufSize;

    snd_buf = new(nothrow) char[size_buf];
    if (!snd_buf)
    {
        print_err("[%d]<%s:%d> Error malloc()\n", num_chld, __func__, __LINE__);
        exit(1);
    }

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

        FD_ZERO(&rdfds);
        FD_ZERO(&wrfds);
        set_list();
        int ret = select_(num_chld, ReqMan);
        if (ret < 0)
        {
            print_err("[%d]<%s:%d> Error poll_()\n", num_chld, __func__, __LINE__);
            break;
        }
    }

    delete[] snd_buf;
}
//======================================================================
static void add_wait_list(Connect *r)
{
    r->io_status = WORK;
    r->sock_timer = 0;
    r->next = NULL;
mtx_.lock();
    r->prev = wait_list_end;
    if (wait_list_start)
    {
        wait_list_end->next = r;
        wait_list_end = r;
    }
    else
        wait_list_start = wait_list_end = r;
mtx_.unlock();
    cond_.notify_one();
}
//======================================================================
void push_send_file(Connect* r)
{
    r->io_direct = TO_CLIENT;
    r->source_entity = FROM_FILE;
    r->operation = SEND_RESP_HEADERS;
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();

    _lseeki64(r->resp.fd, r->resp.offset, SEEK_SET);
    add_wait_list(r);
}
//======================================================================
void push_pollin_list(Connect* r)
{
    r->io_direct = FROM_CLIENT;
    r->operation = READ_REQUEST;
    add_wait_list(r);
}
//======================================================================
void push_send_multipart(Connect *r)
{
    r->io_direct = TO_CLIENT;
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();

    r->source_entity = MULTIPART_ENTITY;
    r->operation = SEND_RESP_HEADERS;
    add_wait_list(r);
}
//======================================================================
void push_send_html(Connect* r)
{
    r->io_direct = TO_CLIENT;
    r->source_entity = FROM_DATA_BUFFER;
    r->operation = SEND_RESP_HEADERS;
    add_wait_list(r);
}
//======================================================================
void close_event_handler(void)
{
    close_thr = 1;
    cond_.notify_one();
}
//======================================================================
static int send_html(Connect* r)
{
    int ret = send(r->clientSocket, r->html.p, r->html.len, 0);
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return TRYAGAIN;
        return -1;
    }

    r->html.p += ret;
    r->html.len -= ret;
    r->resp.send_bytes += ret;
    if (r->html.len == 0)
        ret = 0;

    return ret;
}
//======================================================================
static void set_part(Connect *r)
{
    r->mp.status = SEND_HEADERS;

    r->resp_headers.len = create_multipart_head(r);
    r->resp_headers.p = r->mp.hdr.c_str();

    r->resp.offset = r->mp.rg->start;
    r->resp.respContentLength = r->mp.rg->len;
    lseek(r->resp.fd, r->resp.offset, SEEK_SET);
}
//======================================================================
static void worker(Connect* r, RequestManager* ReqMan)
{
    if (r->operation == SEND_ENTITY)
    {
        if (r->source_entity == FROM_FILE)
        {
            int wr = send_entity(r);
            if (wr == 0)
            {
                del_from_list(r);
                end_response(r);
            }
            else if (wr < 0)
            {
                if (wr == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    r->err = wr;
                    r->req_hdrs.iReferer = MAX_HEADERS - 1;
                    r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                    del_from_list(r);
                    end_response(r);
                }
            }
            else
                r->sock_timer = 0;
        }
        else if (r->source_entity == MULTIPART_ENTITY)
        {
            if (r->mp.status == SEND_HEADERS)
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
                        del_from_list(r);
                        end_response(r);
                    }
                }
                else if (wr > 0)
                {
                    r->resp_headers.p += wr;
                    r->resp_headers.len -= wr;
                    r->resp.send_bytes += wr;
                    if (r->resp_headers.len == 0)
                    {
                        r->mp.status = SEND_PART;
                    }
                    r->sock_timer = 0;
                }
            }
            else if (r->mp.status == SEND_PART)
            {
                int wr = send_entity(r);
                if (wr == 0)
                {
                    r->sock_timer = 0;
                    r->mp.rg = r->rg.get();
                    if (r->mp.rg)
                    {
                        set_part(r);
                    }
                    else
                    {
                        r->mp.status = SEND_END;
                        r->mp.hdr = "";
                        r->mp.hdr << "\r\n--" << boundary << "--\r\n";
                        r->resp_headers.len = r->mp.hdr.size();
                        r->resp_headers.p = r->mp.hdr.c_str();
                    }
                }
                else if (wr < 0)
                {
                    if (wr == TRYAGAIN)
                        r->io_status = SELECT;
                    else
                    {
                        r->err = wr;
                        r->req_hdrs.iReferer = MAX_HEADERS - 1;
                        r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                        del_from_list(r);
                        end_response(r);
                    }
                }
            }
            else if (r->mp.status == SEND_END)
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
                        del_from_list(r);
                        end_response(r);
                    }
                }
                else if (wr > 0)
                {
                    r->resp_headers.p += wr;
                    r->resp_headers.len -= wr;
                    r->resp.send_bytes += wr;
                    if (r->resp_headers.len == 0)
                    {
                        del_from_list(r);
                        end_response(r);
                    }
                    else
                        r->sock_timer = 0;
                }
            }
        }
        else if (r->source_entity == FROM_DATA_BUFFER)
        {
            int wr = send_html(r);
            if (wr == 0)
            {
                del_from_list(r);
                end_response(r);
            }
            else if (wr < 0)
            {
                if (wr == TRYAGAIN)
                    r->io_status = SELECT;
                else
                {
                    r->err = -1;
                    r->req_hdrs.iReferer = MAX_HEADERS - 1;
                    r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                    del_from_list(r);
                    end_response(r);
                }
            }
            else // (wr > 0)
                r->sock_timer = 0;
        }
    }
    else if (r->operation == SEND_RESP_HEADERS)
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
                del_from_list(r);
                end_response(r);
            }
        }
        else if (wr > 0)
        {
            r->resp_headers.p += wr;
            r->resp_headers.len -= wr;
            if (r->resp_headers.len == 0)
            {
                if (r->reqMethod == M_HEAD)
                {
                    del_from_list(r);
                    end_response(r);
                }
                else
                {
                    r->sock_timer = 0;
                    if (r->source_entity == FROM_DATA_BUFFER)
                    {
                        if (r->html.len == 0)
                        {
                            del_from_list(r);
                            end_response(r);
                        }
                        else
                        {
                            r->operation = SEND_ENTITY;
                        }
                    }
                    else if (r->source_entity == FROM_FILE)
                    {
                        r->operation = SEND_ENTITY;
                    }
                    else if (r->source_entity == MULTIPART_ENTITY)
                    {
                        if ((r->mp.rg = r->rg.get()))
                        {
                            r->operation = SEND_ENTITY;
                            set_part(r);
                        }
                        else
                        {
                            r->err = -1;
                            del_from_list(r);
                            end_response(r);
                        }
                    }
                }
            }
            else
                r->sock_timer = 0;
        }
    }
    else if (r->operation == READ_REQUEST)
    {
        int ret = r->read_request_headers();
        if (ret < 0)
        {
            if (ret == TRYAGAIN)
                r->io_status = SELECT;
            else
            {
                r->err = -1;
                del_from_list(r);
                end_response(r);
            }
        }
        else if (ret > 0)
        {
            del_from_list(r);
            ReqMan->push_resp_list(r);
        }
        else
            r->sock_timer = 0;
    }
    else
    {
        fprintf(stderr, "<%s:%d> ? operation=%s\n", __func__, __LINE__, get_str_operation(r->operation));
        del_from_list(r);
        end_response(r);
    }
}
