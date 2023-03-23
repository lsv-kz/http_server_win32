#include "classes.h"

using namespace std;
//======================================================================
static Connect *work_list_start = NULL;
static Connect *work_list_end = NULL;

static Connect *wait_list_start = NULL;
static Connect *wait_list_end = NULL;

struct pollfd *cgi_poll_fd;
static Connect **poll_array;

static mutex mtx_;
static condition_variable cond_;

static int close_thr = 0;
static unsigned int num_wait, num_work;

const int MaxCgiProc = 100;

const DWORD PIPE_BUFSIZE = 1024;
const int TimeoutPipe = 10;

static void cgi_set_poll_list(Connect *r, int *i);
int ReadPipe(Connect *r, char* buf, int sizeBuf, int maxRd, int timeout);
int WritePipe(Connect *r, const char* buf, int lenBuf, int sizePipeBuf, int timeout);
int cgi_stdin(Connect *req);
int cgi_stdout(Connect *req);
static void cgi_(Connect* r);
int cgi_find_empty_line(Connect *r);
int cgi_read_hdrs(Connect *req);
static int cgi_poll(int num_chld, int nfd, RequestManager *ReqMan);
int timeout_pipe(Connect *r);
//======================================================================
Cgi::Cgi()
{
    Pipe.timeout = false;
    lenEnv = 0;
    sizeBufEnv = 0;
    bufEnv = NULL;
}
//======================================================================
Cgi::~Cgi()
{
print_err("<%s:%d> sizeBufEnv=%d\n", __func__, __LINE__, sizeBufEnv);
    if (bufEnv)
        delete[] bufEnv;
}
//======================================================================
int Cgi::init(size_t n)
{
    lenEnv = 0;
    Pipe.timeout = false;
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
        if (r->operation > CGI_CONNECT)
        {
			DisconnectNamedPipe(r->cgi.Pipe.parentPipe);
			CloseHandle(r->cgi.Pipe.parentPipe);
			CloseHandle(r->cgi.Pipe.hEvent);
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
static void cgi_set_work_list()
{
mtx_.lock();
    if ((num_work < MaxCgiProc) && wait_list_end)
    {
        int n_max = MaxCgiProc - num_work;
        Connect *r = wait_list_end;

        for ( ; (n_max > 0) && r; r = wait_list_end, --n_max)
        {
            r->cgi.dir = CGI_IN;
            wait_list_end = r->prev;
            if (wait_list_end == NULL)
                wait_list_start = NULL;
            --num_wait;
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
static int cgi_set_poll_list()
{
    int i = 0;
    time_t t = time(NULL);

    Connect *r = work_list_start, *next = NULL;
    for ( ; r; r = next)
    {
        next = r->next;

        if (((t - r->sock_timer) >= r->timeout) && (r->sock_timer != 0))
        {
            print_err(r, "<%s:%d> Timeout = %ld\n", __func__, __LINE__, t - r->sock_timer);
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
                    cgi_set_poll_list(r, &i);
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

    return i;
}
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
int cgi_create_proc(Connect* req)
{
    size_t len;
    struct _stat st;
    BOOL bSuccess;
    String stmp;

    char pipeName[40] = "\\\\.\\pipe\\cgi";
    
    req->cgi.init(1500);

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE childPipe;
    ZeroMemory(&req->cgi.Pipe.oOverlap, sizeof(req->cgi.Pipe.oOverlap));

    wstring commandLine;
    wstring wPath;

    if (req->resp.scriptType == CGI)
    {
        wPath += conf->wCgiDir;
        wPath += cgi_script_file(req->wScriptName);
    }
    else if (req->resp.scriptType == PHPCGI)
    {
        wPath += conf->wRootDir;
        wPath += req->wScriptName;
    }

    if (_wstat(wPath.c_str(), &st) == -1)
    {
        utf16_to_utf8(wPath, stmp);
        print_err(req, "<%s:%d> script (%s) not found: err=%s\n", __func__,
            __LINE__, stmp.c_str(), strerror(errno));
        return -RS404;
    }
    //--------------------- set environment ----------------------------
     {
        const int size = 4096;
        char tmpBuf[size];
        if (GetWindowsDirectory(tmpBuf, size))
        {
            req->cgi.param("SYSTEMROOT", tmpBuf);
        }
        else
        {
            print_err(req, "<%s:%d> Error getenv_s()\n", __func__, __LINE__);
            return -RS500;
        }
    }
  
    {
        const int size = 4096;
        char tmpBuf[size];
        if (ExpandEnvironmentStringsA("PATH=%PATH%", tmpBuf, size))
        {
            req->cgi.param("PATH", tmpBuf);
        }
        else
        {
            print_err(req, "<%s:%d> Error getenv_s()\n", __func__, __LINE__);
            return -RS500;
        }
    }
   
    if (req->reqMethod == M_POST)
    {
        if (req->req_hdrs.iReqContentLength >= 0)
            req->cgi.param("CONTENT_LENGTH", req->req_hdrs.Value[req->req_hdrs.iReqContentLength]);
        else
        {
            //      print_err(req, "<%s:%d> 411 Length Required\n", __func__, __LINE__);
            return -RS411;
        }

        if (req->req_hdrs.reqContentLength > conf->ClientMaxBodySize)
        {
            req->connKeepAlive = 0;
            return -RS413;
        }

        if (req->req_hdrs.iReqContentType >= 0)
            req->cgi.param("CONTENT_TYPE", req->req_hdrs.Value[req->req_hdrs.iReqContentType]);
        else
        {
            print_err(req, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }
    }

    if (req->resp.scriptType == PHPCGI)
        req->cgi.param("REDIRECT_STATUS", "true");
    if (req->req_hdrs.iUserAgent >= 0)
        req->cgi.param("HTTP_USER_AGENT", req->req_hdrs.Value[req->req_hdrs.iUserAgent]);
    if (req->req_hdrs.iReferer >= 0)
        req->cgi.param("HTTP_REFERER", req->req_hdrs.Value[req->req_hdrs.iReferer]);
    if (req->req_hdrs.iHost >= 0)
        req->cgi.param("HTTP_HOST", req->req_hdrs.Value[req->req_hdrs.iHost]);

    req->cgi.param("SERVER_PORT", conf->ServerPort.c_str());

    req->cgi.param("SERVER_SOFTWARE", conf->ServerSoftware.c_str());
    req->cgi.param("GATEWAY_INTERFACE", "CGI/1.1");

    if (req->reqMethod == M_HEAD)
        req->cgi.param("REQUEST_METHOD", get_str_method(M_GET));
    else
        req->cgi.param("REQUEST_METHOD", get_str_method(req->reqMethod));
    
    req->cgi.param("REMOTE_HOST", req->remoteAddr);
    req->cgi.param("SERVER_PROTOCOL", get_str_http_prot(req->httpProt));

    utf16_to_utf8(conf->wRootDir, stmp);
    req->cgi.param("DOCUMENT_ROOT", stmp.c_str());

    req->cgi.param("REQUEST_URI", req->uri);

    utf16_to_utf8(wPath, stmp);
    req->cgi.param("SCRIPT_FILENAME", stmp.c_str());

    //utf16_to_utf8(req->wDecodeUri, stmp);
    //env.add("SCRIPT_NAME", stmp.c_str());
    req->cgi.param("SCRIPT_NAME", req->decodeUri);
    
    req->cgi.param("GGGG_GGGG", "фы ва");

    req->cgi.param("REMOTE_ADDR", req->remoteAddr);
    req->cgi.param("REMOTE_PORT", req->remotePort);
    req->cgi.param("QUERY_STRING", req->sReqParam);
    //------------------------------------------------------------------
    if (req->resp.scriptType == PHPCGI)
    {
        commandLine = conf->wPathPHP_CGI;
    }
    else if (req->resp.scriptType == CGI)
    {
        if (wcsstr(req->wScriptName, L".pl") || wcsstr(req->wScriptName, L".cgi"))
        {
            commandLine = conf->wPerlPath;
            commandLine += L' ';
            commandLine += wPath;
        }
        else if (wcsstr(req->wScriptName, L".py"))
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
        print_err(req, "<%s:%d> Error CommandLine CreateProcess()\n", __func__, __LINE__);
        return -RS500;
    }
    //------------------------------------------------------------------
    req->cgi.Pipe.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (req->cgi.Pipe.hEvent == NULL)
    {
        print_err(req, "<%s:%d> CreateEvent failed with %lu\n", __func__, __LINE__, GetLastError());
        return -RS500;
    }
    req->cgi.Pipe.oOverlap.hEvent = req->cgi.Pipe.hEvent;
    //------------------------------------------------------------------

    len = strlen(pipeName);
    snprintf(pipeName + len, sizeof(pipeName) - len, "%d%d", req->numChld, req->numConn);
    req->cgi.Pipe.parentPipe = CreateNamedPipeA(
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
    if (req->cgi.Pipe.parentPipe == INVALID_HANDLE_VALUE)
    {
        print_err(req, "<%s:%d> CreateNamedPipe failed, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(req->cgi.Pipe.hEvent);
        return -RS500;
    }

    if (!SetHandleInformation(req->cgi.Pipe.parentPipe, HANDLE_FLAG_INHERIT, 0))
    {
        print_err(req, "<%s:%d> Error SetHandleInformation, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(req->cgi.Pipe.hEvent);
        DisconnectNamedPipe(req->cgi.Pipe.parentPipe);
        CloseHandle(req->cgi.Pipe.parentPipe);
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
        print_err(req, "<%s:%d> Error CreateFile, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(req->cgi.Pipe.hEvent);
        DisconnectNamedPipe(req->cgi.Pipe.parentPipe);
        CloseHandle(req->cgi.Pipe.parentPipe);
        return -RS500;
    }

    ConnectNamedPipe(req->cgi.Pipe.parentPipe, &req->cgi.Pipe.oOverlap);

    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));

    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = childPipe;
    si.hStdInput = childPipe;
    si.hStdError = GetHandleLogErr();
    si.dwFlags |= STARTF_USESTDHANDLES;

    bSuccess = CreateProcessW(NULL, (wchar_t*)commandLine.c_str(), NULL, NULL, TRUE, 0, req->cgi.bufEnv, NULL, &si, &pi);
    CloseHandle(childPipe);
    if (!bSuccess)
    {
        utf16_to_utf8(commandLine, stmp);
        print_err(req, "<%s:%d> Error CreateProcessW(%s)\n", __func__, __LINE__, stmp.c_str());
        PrintError(__func__, __LINE__, "Error CreateProcessW()");
        CloseHandle(req->cgi.Pipe.hEvent);
        DisconnectNamedPipe(req->cgi.Pipe.parentPipe);
        CloseHandle(req->cgi.Pipe.parentPipe);
        return -RS500;
    }
    else
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

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
            print_err(r, "<%s:%d> ??????????\n", __func__, __LINE__);
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

        cgi_set_work_list();
        int size_poll_list = cgi_set_poll_list();
        if (size_poll_list > 0)
        {
            if (cgi_poll(num_chld, size_poll_list, ReqMan) < 0)
            	break;
		}

        cgi_worker(num_chld, ReqMan);
    }

    delete [] cgi_poll_fd;
    delete [] poll_array;
}
//======================================================================
void push_cgi(Connect *r)
{
    r->resp.respStatus = RS200;
    r->operation = CGI_CONNECT;
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
static void cgi_set_poll_list(Connect *r, int *i)
{
    if (r->operation == CGI_CONNECT)
    {
        int ret = cgi_create_proc(r);
        if (ret < 0)
        {
            print_err(r, "<%s:%d> Error cgi_create_proc()\n", __func__, __LINE__);
            r->err = ret;
            cgi_del_from_list(r);
            end_response(r);
            return;
        }
        else
        {
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
					}
					else
					{
						r->cgi.dir = CGI_IN;
						r->timeout = conf->TimeOut;
					}
                }
                else if (r->cgi.len_post == 0)
                {
                    r->operation = CGI_STDOUT;
                    r->cgi.status = READ_HEADERS;
                    r->lenTail = 0;
                    r->tail = NULL;
                    r->p_newline = r->cgi.buf;
                    r->cgi.dir = CGI_IN;
					r->timeout = conf->TimeOutCGI;
                }
                else
                {
                    r->err = -RS400;
                    cgi_del_from_list(r);
                    end_response(r);
                }
            }
            else
            {
                r->operation = CGI_STDOUT; 
                r->cgi.status = READ_HEADERS;
                r->lenTail = 0;
                r->tail = NULL;
                r->p_newline = r->cgi.buf;
                r->cgi.dir = CGI_IN;
                r->timeout = conf->TimeOutCGI;
            }

            r->cgi.len_buf = 0;
            r->sock_timer = 0;
        }

        return;
    }
    else if (r->operation == CGI_STDIN)
    {
        if (r->lenTail > 0)
        {
            // write to pipe
            r->poll_status = WORK;
            return;
        }
        else if (r->cgi.dir == CGI_IN)
        {
            cgi_poll_fd[*i].fd = r->clientSocket;
            cgi_poll_fd[*i].events = POLLIN;
            poll_array[*i] = r;
        }
        else if (r->cgi.dir == CGI_OUT)
        {
            // write to pipe
            r->poll_status = WORK;
            return;
        }
    }
    else if (r->operation == CGI_STDOUT)
    {
        if (r->cgi.status == READ_HEADERS)
        {
            // read from pipe
            if (r->cgi.Pipe.timeout == true)
            {
				int ret = timeout_pipe(r);
				if (ret > 0)
				{
					r->lenTail += ret;
					r->cgi.len_buf += ret;
					r->cgi.buf[r->cgi.len_buf] = 0;

					int n = cgi_find_empty_line(r);
					if (n == 1) // empty line found
					{
						r->mode_send = ((r->httpProt == HTTP11) && r->connKeepAlive) ? CHUNK : NO_CHUNK;
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
							r->resp_headers.p = r->resp_headers.s.c_str();
							r->resp_headers.len = r->resp_headers.s.size();
							r->cgi.status = SEND_HEADERS;
							r->sock_timer = 0;
							cgi_poll_fd[*i].fd = r->clientSocket;
							cgi_poll_fd[*i].events = POLLOUT;
							poll_array[*i] = r;
						}
					}
					else if (n < 0) // error
					{
						print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
						r->err = -1;
						cgi_del_from_list(r);
						end_response(r);
						return;
					}
					else
					{
						return;
					}
				}
				else if (ret == 0)
				{
					print_err(r, "<%s:%d> ??? Error timeout_pipe()=0\n", __func__, __LINE__);
					r->err = -1;
					cgi_del_from_list(r);
					end_response(r);
					return;
				}
				else if (ret == -ERROR_IO_INCOMPLETE)
				{
					r->poll_status = WAIT;
					return;
				}
				else if (ret < 0)
				{
					r->err = -RS504;
					cgi_del_from_list(r);
					end_response(r);
					return;
				}
			}
            r->poll_status = WORK;
            return;
        }
        else if (r->cgi.status == SEND_HEADERS)
        {
            cgi_poll_fd[*i].fd = r->clientSocket;
            cgi_poll_fd[*i].events = POLLOUT;
            poll_array[*i] = r;
        }
        else if (r->cgi.status == READ_CONTENT)
        {
            if (r->lenTail > 0)
            {
                if (r->cgi.dir == CGI_IN)
                {
                    int len = (r->lenTail > r->cgi.size_buf) ? r->cgi.size_buf : r->lenTail;
                    memmove(r->cgi.buf + 8, r->tail, len);
                    r->lenTail -= len;
                    if (r->lenTail == 0)
                        r->tail = NULL;
                    else
                        r->tail += len;
                    r->cgi.len_buf = len;
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
                    cgi_poll_fd[*i].fd = r->clientSocket;
                    cgi_poll_fd[*i].events = POLLOUT;
                    poll_array[*i] = r;
                }
            }
            else if (r->cgi.dir == CGI_IN)
            {
                //read from pipe POLLIN
                r->poll_status = WORK;
                return;
            }
            else if (r->cgi.dir == CGI_OUT)
            {
                cgi_poll_fd[*i].fd = r->clientSocket;
                cgi_poll_fd[*i].events = POLLOUT;
                poll_array[*i] = r;
            }
        }
    }
 
    (*i)++;
}
//======================================================================
int ReadPipe(Connect *r, char* buf, int sizeBuf, int maxRd, int timeout)
{
    DWORD dwRead = -1;
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
                    //print_err("<%s:%d> WAIT_OBJECT_0\n", __func__, __LINE__);
                    break;
                case WAIT_TIMEOUT:
                    print_err(r, "<%s:%d> WAIT_TIMEOUT: %d\n", __func__, __LINE__, timeout);
                    r->cgi.Pipe.timeout = true;
                    return -WAIT_TIMEOUT;
                case WAIT_FAILED:
                    print_err(r, "<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    return -1;
                default:
                    //print_err(r, "<%s%d> default: %lu\n", __func__, __LINE__, dwWait);
                    return -1;
            }
        }
        else
            return -1;
    }

    bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, &dwRead, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        //DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            return 0;
        }

        return -1;
    }

    return dwRead;
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
            if (rd < 0)
            {
                r->err = rd;
                cgi_del_from_list(r);
                end_response(r);
            }
            else if (rd > 0)
            {
                r->mode_send = ((r->httpProt == HTTP11) && r->connKeepAlive) ? CHUNK : NO_CHUNK;
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
                    r->resp_headers.p = r->resp_headers.s.c_str();
                    r->resp_headers.len = r->resp_headers.s.size();
                    r->cgi.status = SEND_HEADERS;
                    r->sock_timer = 0;
                }
            }

            return;
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
                            r->cgi.dir = CGI_IN;
                            r->timeout = conf->TimeOutCGI;
                            r->sock_timer = 0;
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
                ;
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
    if (r->tail)
    {
        if ((r->resp.scriptType == CGI) || (r->resp.scriptType == PHPCGI))
        {
            // write to pipe
            int ret = WritePipe(r, r->tail, r->lenTail, PIPE_BUFSIZE, TimeoutPipe);//conf->TimeOutCGI
            if (ret < 0)
            {
                if (ret == -WAIT_TIMEOUT)
                {
                    return 0;
                }
                print_err(r, "<%s:%d> Error write_to_script()=%d\n", __func__, __LINE__, ret);
                return -RS502;
            }
            r->lenTail -= ret;
            r->tail += ret;
            if (r->lenTail == 0)
            {
                r->tail = NULL;
                if (r->cgi.len_post == 0)
                {
                    r->sock_timer = 0;
                    r->operation = CGI_STDOUT;
                    r->cgi.status = READ_HEADERS;
                    r->p_newline = r->cgi.buf;
                    r->cgi.len_buf = 0;
                }
                else
                {
                    r->cgi.dir = CGI_IN;
                    r->timeout = conf->TimeOut;
                    r->sock_timer = 0;
				}
            }
            return 0;
        }
        else
        {
            print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__);
            return -1;
        }
    }
    else if (r->cgi.dir == CGI_IN)
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
            int ret = WritePipe(r, r->cgi.p, r->cgi.len_buf, PIPE_BUFSIZE, TimeoutPipe);//conf->TimeOutCGI
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
                    r->sock_timer = 0;
                    r->operation = CGI_STDOUT;
                    r->cgi.status = READ_HEADERS;
                    r->tail = NULL;
                    r->p_newline = r->cgi.buf;
                    r->cgi.len_buf = 0;
                }
                else
                {
                    r->cgi.dir = CGI_IN;
                    r->timeout = conf->TimeOut;
                    r->sock_timer = 0;
				}
            }
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
            r->cgi.len_buf = ReadPipe(r, r->cgi.buf + 8, r->cgi.size_buf, PIPE_BUFSIZE, TimeoutPipe);
            if (r->cgi.len_buf < 0)
            {
                print_err(r, "<%s:%d> Error ReadPipe()=%d\n", __func__, __LINE__, r->cgi.len_buf);
                if (r->cgi.len_buf == -WAIT_TIMEOUT)
                {
                    return 0;
                }

                return -RS502;
            }
            else if (r->cgi.len_buf == 0)
            {
                if (r->mode_send == CHUNK)
                {
                    r->cgi.len_buf = 0;
                    cgi_set_size_chunk(r);
                    r->cgi.dir = CGI_OUT;
                    r->timeout = conf->TimeOut;
                    r->sock_timer = 0;
                    r->mode_send = CHUNK_END;
                    return 4;
                }
                return 0;
            }

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
            return r->cgi.len_buf;
        }
        else
        {
            print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__);
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
                    //print_err(r, "<%s:%d> Error (pCR)\n", __func__, __LINE__);
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
		unsigned int num_read = r->cgi.size_buf - r->cgi.len_buf - 1;
		if (num_read <= 0)
			return -RS505;
        if (num_read > PIPE_BUFSIZE)
			num_read = PIPE_BUFSIZE;

        int n = ReadPipe(r, r->cgi.buf + r->cgi.len_buf, num_read, PIPE_BUFSIZE, TimeoutPipe);
        if (n < 0)
		{
			if (n == -WAIT_TIMEOUT)
				return 0;
			return -1;
		}
		else if (n == 0)
		{
            print_err(r, "<%s:%d> Error ReadPipe=%d\n", __func__, __LINE__, n);
			return -1;
		}

        r->lenTail += n;
		r->cgi.len_buf += n;
		r->cgi.buf[r->cgi.len_buf] = 0;
		n = cgi_find_empty_line(r);
		if (n == 1) // empty line found
			return r->cgi.len_buf;
		else if (n < 0) // error
			return n;
    }
    else
    {
        print_err(r, "<%s:%d> ??? scriptType=%d\n", __func__, __LINE__);
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
int timeout_pipe(Connect *r)
{
	DWORD ready_bytes;
    bool bSuccess = GetOverlappedResult(r->cgi.Pipe.parentPipe, &r->cgi.Pipe.oOverlap, &ready_bytes, false);
    if (!bSuccess)
    {
        DWORD err = GetLastError();
        //DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
        if (err == ERROR_BROKEN_PIPE) // 109
        {
            return 0;
        }
        else if (err == ERROR_IO_INCOMPLETE) // 996
        {
            return -ERROR_IO_INCOMPLETE;
        }
        return -1;
    }
    if (ready_bytes > 0)
		r->cgi.Pipe.timeout = false;
    return ready_bytes;
}
