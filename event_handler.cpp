#include "main.h"

// POLLERR=0x1, POLLHUP=0x2, POLLNVAL=0x4, POLLPRI=0x400, POLLRDBAND=0x200
// POLLRDNORM=0x100, POLLWRNORM=0x10, POLLIN=0x300, POLLOUT=0x10
// 0x13, 0x2, 0x12
using namespace std;

static Connect* work_list_start = NULL;
static Connect* work_list_end = NULL;

static Connect* wait_list_start = NULL;
static Connect* wait_list_end = NULL;

static struct pollfd* pollfd_array;

static mutex mtx_;
static condition_variable cond_;

static int close_thr = 0;
static int size_buf;
static char* snd_buf;

int send_html(Connect* r);
//======================================================================
int send_entity(Connect* req)//, char* rd_buf, int size_buf
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

    ret = send_file_2(req->clientSocket, req->resp.fd, snd_buf, len);
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
void del_from_list(Connect* r)
{
    if ((r->source_entity == FROM_FILE) && (r->event == POLLWRNORM))
        _close(r->resp.fd);
    else
        get_time(r->resp.sTime);

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
int set_list()
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

    int i = 0;
    __time64_t t = _time64(NULL);
    Connect* r = work_list_start, *next;

    for (; r; r = next)
    {
        next = r->next;

        if (((int)(t - r->sock_timer) >= r->timeout) && (r->sock_timer != 0))
        {
            if (r->operation != READ_REQUEST)
            {
                r->err = -1;
                print_err(r, "<%s:%d> Timeout = %ld\n", __func__, __LINE__, (long)(t - r->sock_timer));
                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Timeout";
            }

            del_from_list(r);
            end_response(r);
        }
        else
        {
            if (r->sock_timer == 0)
                r->sock_timer = t;

            pollfd_array[i].fd = r->clientSocket;
            pollfd_array[i].events = r->event;
            ++i;
        }
    }

    return i;
}
//======================================================================
int poll_(int num_chld, int nfd, RequestManager* ReqMan)
{
    int ret = WSAPoll(pollfd_array, nfd, conf->TimeoutPoll);
    if (ret == -1)
    {
        print_err("[%d]<%s:%d> Error poll()\n", num_chld, __func__, __LINE__);
        return -1;
    }
    else if (ret == 0)
        return 0;

    Connect* r = work_list_start, * next = NULL;
    for ( int i = 0; (i < nfd) && (ret > 0) && r; r = next, ++i)
    {
        next = r->next;
        if (pollfd_array[i].revents == POLLWRNORM)
        {
            --ret;
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
                        if (wr != TRYAGAIN)
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
                        if (wr != TRYAGAIN)
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
                    int err = ErrorStrSock(__func__, __LINE__, "Error send()");
                    if (err != WSAEWOULDBLOCK)
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
                            if ((r->source_entity == FROM_DATA_BUFFER) && (r->html.len == 0))
                            {
                                del_from_list(r);
                                end_response(r);
                            }
                            else
                            {
                                r->operation = SEND_ENTITY;
                                r->sock_timer = 0;
                            }
                        }
                    }
                    else
                    {
                        r->sock_timer = 0;
                    }
                }
            }
            else
            {
                print_err("<%s:%d> ? r->operation=%d\n", __func__, __LINE__, r->operation);
            }
        }
        else if (pollfd_array[i].revents == POLLRDNORM)
        {
            --ret;
            int n = r->hd_read();
            if (n < 0)
            {
                if (n != TRYAGAIN)
                {
                    r->err = n;
                    del_from_list(r);
                    end_response(r);
                }
            }
            else if (n > 0)
            {
                del_from_list(r);
                push_resp_list(r, ReqMan);
            }
            else
                r->sock_timer = 0;
        }
        else if (pollfd_array[i].revents)
        {
            --ret;
            //print_err(r, "<%s:%d> Error: events=0x%x, revents=0x%x\n", __func__, __LINE__, pollfd_array[i].events, pollfd_array[i].revents);
            if (r->event == POLLWRNORM)
            {
                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                r->err = -1;
            }
            else
                r->err = -1;

            del_from_list(r);
            end_response(r);
        }
    }

    return 1;
}
//======================================================================
void event_handler(RequestManager* ReqMan)
{
    int num_chld = ReqMan->get_num_chld();
    int count_resp = 0;
    size_buf = conf->SndBufSize;

    pollfd_array = new(nothrow) WSAPOLLFD[conf->MaxRequests];
    snd_buf = new(nothrow) char[size_buf];
    if (!snd_buf || !pollfd_array)
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

        count_resp = set_list();
        if (count_resp == 0)
            continue;

        int ret = poll_(num_chld, count_resp, ReqMan);
        if (ret < 0)
        {
            print_err("[%d]<%s:%d> Error poll_()\n", num_chld, __func__, __LINE__);
            break;
        }
    }

    delete[] snd_buf;
    delete[] pollfd_array;
}
//======================================================================
void push_send_file(Connect* r)
{
    r->event = POLLWRNORM;
    r->source_entity = FROM_FILE;
    r->operation = SEND_RESP_HEADERS;
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();

    _lseeki64(r->resp.fd, r->resp.offset, SEEK_SET);
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
void push_send_html(Connect* r)
{
    r->event = POLLWRNORM;
    r->source_entity = FROM_DATA_BUFFER;
    r->operation = SEND_RESP_HEADERS;
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
void push_pollin_list(Connect* r)
{
    r->event = POLLRDNORM;
    r->operation = READ_REQUEST;
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
void close_event_handler(void)
{
    close_thr = 1;
    cond_.notify_one();
}
//======================================================================
int send_html(Connect* r)
{
    int ret = send(r->clientSocket, r->html.p, r->html.len, 0);
    if (ret == SOCKET_ERROR)
    {
        int err = ErrorStrSock(__func__, __LINE__, "Error send()");
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

