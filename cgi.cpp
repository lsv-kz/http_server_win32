#include "classes.h"

using namespace std;
//======================================================================
static Connect *work_list_start = NULL;
static Connect *work_list_end = NULL;

static Connect *wait_list_start = NULL;
static Connect *wait_list_end = NULL;

struct pollfd *cgi_poll_fd;
static Connect **poll_array;
static Connect **io_pipe_array;

static mutex mtx_;
static condition_variable cond_;

static int close_thr = 0;
static unsigned int num_wait, num_work;

const int MaxCgiProc = 100;

const DWORD PIPE_BUFSIZE = 1024;
const int TimeoutPipe = 10;

static void cgi_set_poll_list(Connect *r, int *nsock, int *npipe);
int ReadPipe(Connect *r, char* buf, int sizeBuf, DWORD *nread, int maxRd, int timeout);
int WritePipe(Connect *r, const char* buf, int lenBuf, int sizePipeBuf, int timeout);
int cgi_stdin(Connect *req);
int cgi_stdout(Connect *req);
static void cgi_(Connect* r);
int cgi_find_empty_line(Connect *r);
int cgi_read_hdrs(Connect *req);
static int cgi_poll(int num_chld, int nfd, RequestManager *ReqMan);
void timeout_pipe();
void cgi_set_status_readheaders(Connect *r);
void cgi_set_status_sendheaders(Connect *r);
int cgi_create_proc(Connect* req);
int cgi_set_size_chunk(Connect *r);
//======================================================================
wstring cgi_script_file(const wstring& name)
{
    const wchar_t* p;

    if ((p = wcschr(name.c_str() + 1, '/')))
    {
        return p;
    }
    return name;
}
//======================================================================
Cgi::Cgi()
{
    Pipe.timeout = false;
    Pipe.parentPipe = INVALID_HANDLE_VALUE;
    Pipe.hEvent = INVALID_HANDLE_VALUE;
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
    Pipe.timeout = false;
    Pipe.parentPipe = INVALID_HANDLE_VALUE;
    Pipe.hEvent = INVALID_HANDLE_VALUE;
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
    if ((r->resp.scriptType == CGI) || 
        (r->resp.scriptType == PHPCGI))
    {
        if (r->cgi.Pipe.hEvent != INVALID_HANDLE_VALUE)
            CloseHandle(r->cgi.Pipe.hEvent);
        
        if (r->cgi.Pipe.parentPipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
            CloseHandle(r->cgi.Pipe.parentPipe);
        }
    }

    r->wScriptName = NULL;

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
mtx_.lock();
    --num_work;
mtx_.unlock();
}
//======================================================================
static void cgi_add_work_list()
{
mtx_.lock();
    if ((num_work < MaxCgiProc) && wait_list_end)
    {
        int n_max = MaxCgiProc - num_work;
        Connect *r = wait_list_end;

        for ( ; (n_max > 0) && r; r = wait_list_end, --n_max)
        {
            wait_list_end = r->prev;
            if (wait_list_end == NULL)
                wait_list_start = NULL;
            --num_wait;
            
            int err = cgi_create_proc(r);
            if (err != 0)
            {
                print_err(r, "<%s:%d> Error cgi_create_proc()\n", __func__, __LINE__);
                r->err = err;
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
static void cgi_set_poll_list(int *nsock, int *npipe)
{
    time_t t = time(NULL);
    *nsock = *npipe = 0;
    Connect *r = work_list_start, *next = NULL;
    for ( ; r; r = next)
    {
        next = r->next;

        if (((t - r->sock_timer) >= r->timeout) && (r->sock_timer != 0))
        {
            print_err(r, "<%s:%d> Timeout = %ld, op=%d, stat=%d, dir=%d\n", 
                    __func__, __LINE__, t - r->sock_timer, r->operation, r->cgi.status, r->cgi.dir);
            if ((r->operation == CGI_STDIN) && (r->cgi.dir == CGI_OUT))
            {
				r->err = -RS504;
			}
            else if ((r->operation == CGI_STDOUT) && (r->cgi.status == READ_HEADERS))
			{
				r->err = -RS504;
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
            if (r->sock_timer == 0)
                r->sock_timer = t;
            
            switch (r->resp.scriptType)
            {
                case CGI:
                case PHPCGI:
                    cgi_set_poll_list(r, nsock, npipe);
                    break;
                case PHPFPM:
                case FASTCGI:
                    //fcgi_set_poll_list(r, &i);
                    //break;
                case SCGI:
                    //scgi_set_poll_list(r, &i);
                    //break;
                default:
                    print_err(r, "<%s:%d> ??? cgi.scriptType=%d\n", __func__, __LINE__, r->resp.scriptType);
                    r->err = -RS500;
                    cgi_del_from_list(r);
                    end_response(r);
                    break;
            }
        }
    }
}
//======================================================================
static void cgi_worker(int num_chld, RequestManager *ReqMan)
{
    Connect *r = work_list_start, *next;
    for ( ; r; r = next)
    {
        next = r->next;
        if (r->poll_status == WAIT)
            continue;

        if ((r->resp.scriptType == CGI) || 
            (r->resp.scriptType == PHPCGI))
        {
            cgi_(r);
        }
        /*else if ((r->resp.scriptType == PHPFPM) || 
            (r->resp.scriptType == FASTCGI))
        {
            fcgi_(r);
        }
        else if (r->resp.scriptType == SCGI)
        {
            scgi_(r);
        }*/
        else
        {
            print_err(r, "<%s:%d> ??? Error operation=%d\n", __func__, __LINE__, r->operation);
            cgi_del_from_list(r);
            end_response(r);
        }
    }
}
//======================================================================
void cgi_handler(RequestManager *ReqMan)
{
    int num_chld = ReqMan->get_num_chld();

    cgi_poll_fd = new(nothrow) struct pollfd [conf->MaxRequests];
    if (!cgi_poll_fd)
    {
        print_err("[%d]<%s:%d> Error malloc()\n", num_chld, __func__, __LINE__);
        exit(1);
    }
    
    poll_array = new(nothrow) Connect* [conf->MaxRequests];
    if (!poll_array)
    {
        print_err("[%d]<%s:%d> Error malloc()\n", num_chld, __func__, __LINE__);
        exit(1);
    }
    
    io_pipe_array = new(nothrow) Connect* [conf->MaxRequests];
    if (!io_pipe_array)
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

        int n_sock, n_pipe;
        cgi_add_work_list();
        cgi_set_poll_list( &n_sock, &n_pipe);
        if (n_sock > 0)
        {
            if (cgi_poll(num_chld, n_sock, ReqMan) < 0)
            	break;
		}

        cgi_worker(num_chld, ReqMan);
        timeout_pipe();
    }

    delete [] cgi_poll_fd;
    delete [] poll_array;
    delete [] io_pipe_array;
}
//======================================================================
void push_cgi(Connect *r)
{
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
static void cgi_set_poll_list(Connect *r, int *nsock, int *npipe)
{
    if (r->operation == CGI_STDIN)
    {
        if (r->cgi.dir == CGI_IN)
        {
            cgi_poll_fd[*nsock].fd = r->clientSocket;
            cgi_poll_fd[*nsock].events = POLLIN;
            poll_array[*nsock] = r;
            (*nsock)++;
        }
        else if (r->cgi.dir == CGI_OUT)
        {
            // write to pipe
            r->poll_status = WORK;
            (*npipe)++;
        }
    }
    else if (r->operation == CGI_STDOUT)
    {
        if (r->cgi.status == READ_HEADERS)
        {
            // read from pipe
            if (r->cgi.Pipe.timeout == true)
            {
				r->poll_status = WAIT;
			}
            else
            {
                r->poll_status = WORK;
                (*npipe)++;
            }
        }
        else if (r->cgi.status == SEND_HEADERS)
        {
            cgi_poll_fd[*nsock].fd = r->clientSocket;
            cgi_poll_fd[*nsock].events = POLLOUT;
            poll_array[*nsock] = r;
            (*nsock)++;
        }
        else if (r->cgi.status == READ_CONTENT)
        {
            if (r->cgi.dir == CGI_IN)
            {
                //read from pipe
                if (r->cgi.Pipe.timeout == true)
                {
                    r->poll_status = WAIT;
                }
                else
                {
                    r->poll_status = WORK;
                    (*npipe)++;
                }
            }
            else if (r->cgi.dir == CGI_OUT)
            {
                cgi_poll_fd[*nsock].fd = r->clientSocket;
                cgi_poll_fd[*nsock].events = POLLOUT;
                poll_array[*nsock] = r;
                (*nsock)++;
                
            }
        }
    }
}
//======================================================================
static void cgi_(Connect* r)
{
    if (r->operation == CGI_STDIN)
    {
        int n = cgi_stdin(r);
        if (n < 0)
        {
            print_err(r, "<%s:%d> Error cgi_stdin\n", __func__, __LINE__);
            r->err = n;
            cgi_del_from_list(r);
            end_response(r);
        }
        return;
    }
    else if (r->operation == CGI_STDOUT)
    {
        if (r->cgi.status == READ_HEADERS)
        {
            int rd = cgi_read_hdrs(r);
            if (rd == -WAIT_TIMEOUT)
            {
                ;
            }
            else if (rd < 0)
            {
                r->err = rd;
                cgi_del_from_list(r);
                end_response(r);
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
        }
        else if (r->cgi.status == SEND_HEADERS)
        {
            if (r->resp_headers.len > 0)
            {
                int wr = send(r->clientSocket, r->resp_headers.p, r->resp_headers.len, 0);
                if (wr == SOCKET_ERROR)
                {
                    ErrorStrSock(__func__, __LINE__, "Error send()");
                    
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
                            r->cgi.status = READ_CONTENT;
                            r->sock_timer = 0;
                            if (r->lenTail > 0)
                            {
                                if (r->lenTail > r->cgi.size_buf)
                                {
                                    print_err(r, "<%s:%d> ??? (r->lenTail > r->cgi.size_buf)\n", __func__, __LINE__);
                                    r->err = -1;
                                    cgi_del_from_list(r);
                                    end_response(r);
                                    return;
                                }

                                memmove(r->cgi.buf + 8, r->tail, r->lenTail);
                                r->cgi.len_buf = r->lenTail;
                                r->tail = NULL;
                                r->lenTail = 0;
                                if (r->mode_send == CHUNK)
                                {
                                    if (cgi_set_size_chunk(r))
                                    {
                                        r->err = -1;
                                        cgi_del_from_list(r);
                                        end_response(r);
                                        return;
                                    }
                                }
                                else
                                    r->cgi.p = r->cgi.buf + 8;
                                r->cgi.dir = CGI_OUT;
                                r->timeout = conf->TimeOut;
                            }
                            else
                            {
                                r->cgi.len_buf = 0;
                                r->cgi.p = NULL;
                                r->cgi.dir = CGI_IN;
                                r->timeout = conf->TimeOutCGI;
                            }
                        }
                    }
                    else
                        r->sock_timer = 0;
                }
                return;
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
        else if (r->cgi.status == READ_CONTENT)
        {
            int ret = cgi_stdout(r);
            if (ret == -WAIT_TIMEOUT)
            {
                //print_err(r, "<%s:%d> Error pipe.timeout=%d\n", __func__, __LINE__, r->cgi.Pipe.timeout);
                r->poll_status = WAIT;
            }
            else if (ret < 0)
            {
                r->err = -1;
                cgi_del_from_list(r);
                end_response(r);
            }
            else if (ret == 0)
            {
                cgi_del_from_list(r);
                end_response(r);
            }
        }
    }
    else
    {
        print_err(r, "<%s:%d> ??? Error operation=%d\n", __func__, __LINE__, r->operation);
        r->err = -1;
        cgi_del_from_list(r);
        end_response(r);
    }
}
//======================================================================
int cgi_stdin(Connect *r)
{
    if (r->cgi.dir == CGI_IN)
    {
        int rd = (r->cgi.len_post > r->cgi.size_buf) ? r->cgi.size_buf : r->cgi.len_post;
        r->cgi.len_buf = recv(r->clientSocket, r->cgi.buf, rd, 0);
        if (r->cgi.len_buf == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error recv()");
            return -1;
        }
        else if (r->cgi.len_buf == 0)
        {
            print_err(r, "<%s:%d> Error recv()=0\n", __func__, __LINE__);
            return -1;
        }
        
        r->cgi.len_post -= r->cgi.len_buf;
        r->cgi.p = r->cgi.buf;
        r->cgi.dir = CGI_OUT;
        r->timeout = conf->TimeOutCGI;
        r->sock_timer = 0;
    }
    else if (r->cgi.dir == CGI_OUT)
    {
        if ((r->resp.scriptType == CGI) || (r->resp.scriptType == PHPCGI))
        {
            // write to pipe
            int ret = WritePipe(r, r->cgi.p, r->cgi.len_buf, PIPE_BUFSIZE, TimeoutPipe);
            if (ret < 0)
            {
                if (ret == -WAIT_TIMEOUT)
                {
                    return 0;
                }
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
                    r->cgi.dir = CGI_IN;
                    r->timeout = conf->TimeOut;
				}
            }
            r->sock_timer = 0;
            return 0;
        }
        else
        {
            print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__, r->resp.scriptType);
            return -1;
        }
    }
    
    return 0;
}
//======================================================================
int cgi_stdout(Connect *r)
{
    if (r->cgi.dir == CGI_IN)
    {
        if ((r->resp.scriptType == CGI) || (r->resp.scriptType == PHPCGI))
        {
            // read from pipe
            DWORD read_bytes = 0;
            int ret = ReadPipe(r, r->cgi.buf + 8, r->cgi.size_buf, &read_bytes, PIPE_BUFSIZE, TimeoutPipe);
            if (ret < 0)
            {
                if (ret == -WAIT_TIMEOUT)
                {
                    return -WAIT_TIMEOUT;
                }
                print_err(r, "<%s:%d> ! Error ReadPipe()=%d, read_bytes=%d\n", __func__, __LINE__, ret, read_bytes);
                return -RS502;
            }
            else if (ret == 0)
            {
				if (r->mode_send == CHUNK)
				{
					r->cgi.len_buf = 0;
					cgi_set_size_chunk(r);
					r->cgi.dir = CGI_OUT;
					r->timeout = conf->TimeOut;
					r->sock_timer = 0;
					r->mode_send = CHUNK_END;
                    r->sock_timer = 0;
					return 4;
				}
                return 0;
            }
            r->cgi.len_buf = read_bytes;
            r->cgi.dir = CGI_OUT;
            r->timeout = conf->TimeOut;
            r->sock_timer = 0;
            if (r->mode_send == CHUNK)
            {
                if (cgi_set_size_chunk(r))
                    return -1;
            }
            else
                r->cgi.p = r->cgi.buf + 8;
            r->sock_timer = 0;
            return r->cgi.len_buf;
        }
        else
        {
            print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__, r->resp.scriptType);
            return -1;
        }
    }
    else if (r->cgi.dir == CGI_OUT)
    {
        int ret = send(r->clientSocket, r->cgi.p, r->cgi.len_buf, 0);
        if (ret == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error send()");
            return -1;
        }

        r->cgi.p += ret;
        r->cgi.len_buf -= ret;
        r->resp.send_bytes += ret;
        if (r->resp.send_bytes > 2000) 
            print_err(r, "<%s:%d> r->resp.send_bytes=%d\n", __func__, __LINE__, r->resp.send_bytes);
        if (r->cgi.len_buf == 0)
        {
            if (r->mode_send == CHUNK_END)
                return 0;
            else
            {
                r->cgi.dir = CGI_IN;
                r->timeout = conf->TimeOutCGI;
                r->sock_timer = 0;
			}
        }
        r->sock_timer = 0;
        return ret;
    }

    return 0;
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
    if ((r->resp.scriptType == CGI) || (r->resp.scriptType == PHPCGI))
    {
		// read_from_pipe;
		unsigned int len_read = r->cgi.size_buf - r->cgi.len_buf - 1;
		if (len_read <= 0)
			return -RS505;
        if (len_read > PIPE_BUFSIZE)
			len_read = PIPE_BUFSIZE;
        DWORD read_bytes;
        int ret = ReadPipe(r, r->cgi.buf + r->cgi.len_buf, len_read, &read_bytes, PIPE_BUFSIZE, TimeoutPipe);
        if (ret < 0)
		{
			if (ret == -WAIT_TIMEOUT)
				return -WAIT_TIMEOUT;
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
            r->cgi.buf[r->cgi.len_buf] = 0;
            ret = cgi_find_empty_line(r);
            if (ret == 1) // empty line found
                return r->cgi.len_buf;
            else if (ret < 0) // error
                return ret;
        }
    }
    else
    {
        print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__, r->resp.scriptType);
        return -1;
    }

    return 0;
}
//======================================================================
static int cgi_poll(int num_chld, int nfd, RequestManager *ReqMan)
{
    int ret = WSAPoll(cgi_poll_fd, nfd, conf->TimeoutPoll);
    if (ret == SOCKET_ERROR)
    {
        ErrorStrSock(__func__, __LINE__, "Error WSAPoll()");
        return -1;
    }
    else if (ret == 0)
    {
        //print_err("[%d]<%s:%d> poll()=0\n", num_chld, __func__, __LINE__);
        return 0;
    }

    int i = 0;
    for ( ; (i < nfd) && (ret > 0); ++i)
    {
        Connect *r = poll_array[i];
        if (cgi_poll_fd[i].revents == POLLWRNORM)
        {
            r->poll_status = WORK;
            --ret;
        }
        else if (cgi_poll_fd[i].revents & POLLRDNORM)
        {
            r->poll_status = WORK;
            --ret;
        }
        else if (cgi_poll_fd[i].revents)
        {
            --ret;

            if (cgi_poll_fd[i].fd == r->clientSocket)
            {
                print_err(r, "<%s:%d> Error: events=0x%x(0x%x), operation=%d, send_bytes=%lld\n", 
                        __func__, __LINE__, cgi_poll_fd[i].events, cgi_poll_fd[i].revents, r->operation, r->resp.send_bytes);
                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                r->err = -1;
                cgi_del_from_list(r);
                end_response(r);
                continue;
            }
            else
            {
                switch (r->resp.scriptType)
                {
                    case CGI:
                    case PHPCGI:
                    case SCGI:
                        if ((r->operation == CGI_STDOUT) && (r->cgi.dir == CGI_IN))
                        {
                            if (r->mode_send == CHUNK)
                            {
                                r->cgi.len_buf = 0;
                                cgi_set_size_chunk(r);
                                r->cgi.dir = CGI_OUT;
                                r->timeout = conf->TimeOut;
                                r->sock_timer = 0;
                                r->mode_send = CHUNK_END;
                                continue;
                            }
                            else
                            {
                                cgi_del_from_list(r);
                                end_response(r);
                            }
                        }
                        else
                        {
                            print_err(r, "<%s:%d> Error: events=0x%x(0x%x), operation=%d, send_bytes=%lld\n", 
                                    __func__, __LINE__, cgi_poll_fd[i].events, cgi_poll_fd[i].revents, r->operation, 
                                    r->resp.send_bytes);
                            if (r->operation < CGI_STDOUT)
                            {
                                r->req_hdrs.iReferer = MAX_HEADERS - 1;
                                r->req_hdrs.Value[r->req_hdrs.iReferer] = "Connection reset by peer";
                                r->err = -RS502;
                            }
                            else
                                r->err = -1;
                            cgi_del_from_list(r);
                            end_response(r);
                        }
                        break;
                    case PHPFPM:
                    case FASTCGI:
                        /*if (cgi_poll_fd[i].fd == r->clientSocket)
                        {
                            print_err(r, "<%s:%d> Error: events=0x%x(0x%x), operation=%d\n", 
                                    __func__, __LINE__, cgi_poll_fd[i].events, cgi_poll_fd[i].revents, r->operation);
                            r->req_hd.iReferer = MAX_HEADERS - 1;
                            r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                            r->err = -1;
                        }
                        else if (r->operation < CGI_STDOUT)
                            r->err = -RS502;
                        cgi_del_from_list(r);
                        end_response(r);
                        break;*/
                    default:
                        print_err(r, "<%s:%d> ??? cgi.scriptType=%d\n", __func__, __LINE__, r->resp.scriptType);
                        r->err = -1;
                        cgi_del_from_list(r);
                        end_response(r);
                        break;
                }
            }
        }
        else
        {
            r->poll_status = WAIT;
        }
    }

    return i;
}
//======================================================================
int get_ov_result(Connect *r, DWORD *read_bytes)
{
    *read_bytes = 0;
    bool bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, read_bytes, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        //DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            //print_err(r, "<%s:%d> ------ERROR_BROKEN_PIPE-----\n", __func__, __LINE__);
            if (*read_bytes > 0)
                print_err(r, "<%s:%d> ??? ready_bytes=%u [ERROR_BROKEN_PIPE]\n", __func__, __LINE__, *read_bytes);
            return 0;
        }
        else if (err == ERROR_IO_INCOMPLETE) // 996
        {
            //print_err(r, "<%s:%d> ------ERROR_IO_INCOMPLETE-----\n", __func__, __LINE__);
            return -ERROR_IO_INCOMPLETE;
        }
        return -1;
    }
    if (read_bytes > 0)
    {
		r->cgi.Pipe.timeout = false;
        r->poll_status = WORK;
    }
    return *read_bytes;
}
//======================================================================
void timeout_pipe()
{
    DWORD ready_bytes;
	Connect *r = work_list_start, *next;
    for ( ; r; r = next)
    {
        next = r->next;
        if (r->cgi.Pipe.timeout == false)
            continue;
        if (r->operation == CGI_STDIN)
        {
            if (r->cgi.dir == CGI_OUT)
            {
                //int ret = get_ov_result(r);
                print_err(r, "<%s:%d> ???\n", __func__, __LINE__);
                r->err = -RS502;
                cgi_del_from_list(r);
                end_response(r);
            }
        }
        else if (r->operation == CGI_STDOUT)
        {
            if (r->cgi.status == READ_HEADERS)
            {
                int ret = get_ov_result(r, &ready_bytes);
                if (ret > 0)
				{
					r->lenTail += ready_bytes;
					r->cgi.len_buf += ready_bytes;
					r->cgi.buf[r->cgi.len_buf] = 0;
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
						r->err = -1;
						cgi_del_from_list(r);
						end_response(r);
					}
                }
                else if (ret == 0)
				{
					print_err(r, "<%s:%d> ??? Error get_ov_result()=0\n", __func__, __LINE__);
					r->err = -1;
					cgi_del_from_list(r);
					end_response(r);
				}
				else if (ret == -ERROR_IO_INCOMPLETE)
				{
					r->poll_status = WAIT;
				}
                else// if (ret < 0)
				{
                    print_err(r, "<%s:%d> Error get_ov_result()=%d\n", __func__, __LINE__, ret);
					r->err = -RS502;
					cgi_del_from_list(r);
					end_response(r);
				}
            }
            else if ((r->cgi.status == READ_CONTENT) && (r->cgi.dir == CGI_IN))
            {
                int ret = get_ov_result(r, &ready_bytes);
                if (ret > 0)
                {
                    r->cgi.len_buf = ready_bytes;
                    r->cgi.dir = CGI_OUT;
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
                    else
                        r->cgi.p = r->cgi.buf + 8;
                }
                else if (ret == 0)
                {
                    if (r->mode_send == CHUNK)
                    {
                        r->cgi.len_buf = 0;
                        cgi_set_size_chunk(r);
                        r->cgi.dir = CGI_OUT;
                        r->timeout = conf->TimeOut;
                        r->sock_timer = 0;
                        r->mode_send = CHUNK_END;
                    }
                    else
                    {
                        cgi_del_from_list(r);
                        end_response(r);
                    }
                }
                else if (ret == -ERROR_IO_INCOMPLETE)
                {
                    r->poll_status = WAIT;
                }
                else
                {
                    r->err = -1;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
        }
    }
}
//======================================================================
void cgi_set_status_readheaders(Connect *r)
{
    r->operation = CGI_STDOUT;
    r->cgi.status = READ_HEADERS;
    r->tail = NULL;
    r->p_newline = r->cgi.buf;
    r->cgi.len_buf = 0;
    r->cgi.buf[0] = 0;
    r->timeout = conf->TimeOutCGI;
}
//======================================================================
void cgi_set_status_sendheaders(Connect *r)
{
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();
    r->cgi.status = SEND_HEADERS;
    r->sock_timer = 0;
    r->timeout = conf->TimeOut;
}
//======================================================================
int cgi_create_proc(Connect* r)
{
    size_t len;
    struct _stat st;
    BOOL bSuccess;
    String stmp;
// 
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

    if (r->resp.scriptType == CGI)
    {
        wPath += conf->wCgiDir;
        wPath += cgi_script_file(r->wScriptName);
    }
    else if (r->resp.scriptType == PHPCGI)
    {
        wPath += conf->wRootDir;
        wPath += r->wScriptName;
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

    if (r->resp.scriptType == PHPCGI)
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
    
    r->cgi.param("GGGG_GGGG", "фы ва");

    r->cgi.param("REMOTE_ADDR", r->remoteAddr);
    r->cgi.param("REMOTE_PORT", r->remotePort);
    r->cgi.param("QUERY_STRING", r->sReqParam);
    //------------------------------------------------------------------
    if (r->resp.scriptType == PHPCGI)
    {
        commandLine = conf->wPathPHP_CGI;
    }
    else if (r->resp.scriptType == CGI)
    {
        if (wcsstr(r->wScriptName, L".pl") || wcsstr(r->wScriptName, L".cgi"))
        {
            commandLine = conf->wPerlPath;
            commandLine += L' ';
            commandLine += wPath;
        }
        else if (wcsstr(r->wScriptName, L".py"))
        {
            commandLine = conf->wPyPath;
            commandLine += L' ';
            commandLine += wPath;
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
    r->cgi.Pipe.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (r->cgi.Pipe.hEvent == NULL)
    {
        print_err(r, "<%s:%d> CreateEvent failed with %lu\n", __func__, __LINE__, GetLastError());
        return -RS500;
    }
    r->cgi.Pipe.oOverlap.hEvent = r->cgi.Pipe.hEvent;
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
        print_err(r, "<%s:%d> CreateNamedPipe failed, err=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(r->cgi.Pipe.hEvent);
        r->cgi.Pipe.hEvent = INVALID_HANDLE_VALUE;
        return -RS500;
    }

    if (!SetHandleInformation(r->cgi.Pipe.parentPipe, HANDLE_FLAG_INHERIT, 0))
    {
        print_err(r, "<%s:%d> Error SetHandleInformation, err=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(r->cgi.Pipe.hEvent);
        r->cgi.Pipe.hEvent = INVALID_HANDLE_VALUE;
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
        print_err(r, "<%s:%d> Error CreateFile, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(r->cgi.Pipe.hEvent);
        r->cgi.Pipe.hEvent = INVALID_HANDLE_VALUE;
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
        PrintError(__func__, __LINE__, "Error CreateProcessW()");
        CloseHandle(r->cgi.Pipe.hEvent);
        r->cgi.Pipe.hEvent = INVALID_HANDLE_VALUE;
        DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
        CloseHandle(r->cgi.Pipe.parentPipe);
        r->cgi.Pipe.parentPipe = INVALID_HANDLE_VALUE;
        return -RS500;
    }
    else
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    //------------------------------------------------------------------
    if (r->reqMethod == M_POST)
    {
        r->cgi.len_post = r->req_hdrs.reqContentLength - r->lenTail;
        if (r->req_hdrs.reqContentLength > 0)
        {
            r->operation = CGI_STDIN;
            if (r->lenTail > 0)
            {
                r->cgi.dir = CGI_OUT;
                r->timeout = conf->TimeOutCGI;
                r->cgi.p = r->tail;
                r->cgi.len_buf = r->lenTail;
                r->cgi.dir = CGI_OUT;
            }
            else // [r->lenTail == 0]
            {
                r->cgi.dir = CGI_IN;
                r->timeout = conf->TimeOut;
            }
        }
        else //  (r->req_hdrs.reqContentLength == 0)
        {
            cgi_set_status_readheaders(r);
        }
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
    char *p = r->cgi.buf;
    int i = 7;
    p[i--] = '\n';
    p[i--] = '\r';

    for ( ; i >= 0; --i)
    {
        p[i] = hex[size % 16];
        size /= 16;
        if (size == 0)
            break;
    }

    if (size != 0)
        return -1;

    r->cgi.p = r->cgi.buf + i;
    memcpy(r->cgi.buf + 8 + r->cgi.len_buf, "\r\n", 2);
    r->cgi.len_buf += (8 - i + 2);

    return 0;
}
//======================================================================
int ReadPipe(Connect *r, char* buf, int sizeBuf, DWORD *read_bytes, int maxRd, int timeout)
{
	// return:  [1] - Success; [0] - ERROR_BROKEN_PIPE; [-1]; [-WAIT_TIMEOUT] - Timeout.
    bool bSuccess = false;

    DWORD rd = (sizeBuf < maxRd) ? sizeBuf : maxRd;
    bSuccess = ReadFile(r->cgi.Pipe.parentPipe, buf, rd, NULL, &r->cgi.Pipe.oOverlap);
    if (!bSuccess)
    {
        DWORD err = GetLastError();// DWORD err = PrintError(__func__, __LINE__, "Error ReadFile()");
        if (err == ERROR_BROKEN_PIPE)     // 109
            return 0;
        else if (err == ERROR_IO_PENDING) // 997
        {
            DWORD dwWait = WaitForSingleObject(r->cgi.Pipe.hEvent, timeout);
            switch (dwWait)
            {
                case WAIT_OBJECT_0:
                    //print_err(r, "<%s:%d> WAIT_OBJECT_0\n", __func__, __LINE__);
                    break;
                case WAIT_TIMEOUT: // 0x00000102L, 258
                    //print_err(r, "<%s:%d> WAIT_TIMEOUT: %d\n", __func__, __LINE__, timeout);
                    r->cgi.Pipe.timeout = true;
                    return -WAIT_TIMEOUT;
                case WAIT_FAILED:
                    print_err(r, "<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    return -1;
                default:
                    print_err(r, "<%s%d> default: %lu\n", __func__, __LINE__, dwWait);
                    return -1;
            }
        }
        else
            return -1;
    }

    bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, read_bytes, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        //DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            //print_err(r, "<%s%d>----- ERROR_BROKEN_PIPE -----\n", __func__, __LINE__);
            if (*read_bytes > 0)
                print_err(r, "<%s:%d> ??? ready_bytes=%u [ERROR_BROKEN_PIPE]\n", __func__, __LINE__, *read_bytes);
            return 0;
        }
        else if (err == ERROR_IO_INCOMPLETE)
        {
            //print_err(r, "<%s%d>----- ERROR_IO_INCOMPLETE -----\n", __func__, __LINE__);
            r->cgi.Pipe.timeout = true;
            return -WAIT_TIMEOUT;
        }
        else
        {
            print_err(r, "<%s%d>----- ERR=%u -----\n", __func__, __LINE__, err);
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
        //  DWORD err = PrintError(__func__, __LINE__, "Error WriteFile()");
        if (err == ERROR_IO_PENDING)  //  997
        {
            DWORD dwWait = WaitForSingleObject(r->cgi.Pipe.hEvent, timeout);
            switch (dwWait)
            {
                case WAIT_OBJECT_0: // 0x00000000L
                    break;
                case WAIT_TIMEOUT:  // 0x00000102L
                    print_err(r, "<%s:%d> WAIT_TIMEOUT: %d s\n", __func__, __LINE__, timeout);
                    r->cgi.Pipe.timeout = true;
                    return -WAIT_TIMEOUT;
                case WAIT_FAILED:   // (DWORD)0xFFFFFFFF
                    print_err(r, "<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    return -1;
                default:
                    print_err(r, "<%s%d> default: %lu\n", __func__, __LINE__, dwWait);
                    return -1;
            }
        }
        else
            return -1;
    }

    bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, &dwWrite, false);
    if (!bSuccess)
    {
        PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
        return -1;
    }

    return dwWrite;
}
