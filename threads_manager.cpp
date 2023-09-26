#include "main.h"

using namespace std;
//======================================================================
Connect* create_req(int n_proc);

static mutex mtx_conn;
static condition_variable cond_close_conn;

static int count_conn = 0;
//======================================================================
RequestManager::RequestManager(int n)
{
    list_start = list_end = NULL;
    numChld = n;
}
//----------------------------------------------------------------------
int RequestManager::get_num_chld(void)
{
    return numChld;
}
//----------------------------------------------------------------------
void RequestManager::push_resp_list(Connect* req)
{
mtx_list.lock();
    req->next = NULL;
    req->prev = list_end;
    if (list_start)
    {
        list_end->next = req;
        list_end = req;
    }
    else
        list_start = list_end = req;

mtx_list.unlock();
    cond_list.notify_one();
}
//----------------------------------------------------------------------
Connect* RequestManager::pop_req()
{
unique_lock<mutex> lk(mtx_list);
    while (list_start == NULL)
    {
        cond_list.wait(lk);
    }

    Connect* req = list_start;
    if (!list_start)
        return NULL;

    if (list_start->next)
    {
        list_start->next->prev = NULL;
        list_start = list_start->next;
    }
    else
        list_start = list_end = NULL;

    return req;
}
//======================================================================
void end_response(Connect* req)
{
    if ((req->connKeepAlive == 0) || req->err < 0)
    {
        if (req->err <= -RS101) // err < -100
        {
            req->resp.respStatus = -req->err;
            req->err = -1;
            req->connKeepAlive = 0;
            if (send_message(req, NULL) == 1)
                return;
        }

        if (req->operation != READ_REQUEST)
        {
            print_log(req);
        }
        //----------------- close connect ------------------------------
        shutdown(req->clientSocket, SD_BOTH);
        closesocket(req->clientSocket);
        delete req;
    mtx_conn.lock();
        --count_conn;
    mtx_conn.unlock();
        cond_close_conn.notify_all();
    }
    else
    {
        print_log(req);
        req->init();
        req->timeout = conf->TimeoutKeepAlive;
        ++req->numReq;
        push_pollin_list(req);
    }
}
//======================================================================
void start_conn(void)
{
mtx_conn.lock();
    ++count_conn;
mtx_conn.unlock();
}
//======================================================================
int is_maxconn()
{
mtx_conn.lock();
    int n = 0;
    if (count_conn >= conf->MaxWorkConnections)
        n = 1;
mtx_conn.unlock();
    return n;
}
//======================================================================
enum {_pipe_ = 1, _sock_};
static int line_ = 0;
static int numProc;
static unsigned long allConn = 0;
static SOCKET sock;
static HANDLE pIn_, pOut_;

void close_proc(SOCKET, int);

BOOL WINAPI childSigHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        print_err("%d<%s> signal: Ctrl-C; num conn: %lu; %d\n", numProc, __func__, allConn, line_);
        close_proc(sock, numProc);
        print_err("%d<%s:%d> *** Exit  ***\n", numProc, __func__, __LINE__);
        exit(0);
    }

    return TRUE;
}
//======================================================================
void child_proc(SOCKET sockServer, int numChld, HANDLE pIn, HANDLE pOut)
{
    RequestManager ReqMan(numChld);
    setbuf(stderr, NULL);
    numProc = numChld;
    sock = sockServer;
    pIn_ = pIn;
    pOut_ = pOut;

    if (!SetConsoleCtrlHandler(childSigHandler, TRUE))
    {
        fprintf(stderr, "\nERROR: Could not set control handler\n");
        return;
    }

    WSADATA WsaDat;
    int err = WSAStartup(MAKEWORD(2, 2), &WsaDat);
    if (err != 0)
    {
        print_err("<%s:%d> WSAStartup failed with error: %d\n", __func__, __LINE__, err);
        exit(1);
    }
    //------------------------------------------------------------------
    if (_wchdir(conf->wRootDir.c_str()))
    {
        print_err("%d<%s:%d> Error root_dir: %d\n", numChld, __func__, __LINE__, errno);
        exit(1);
    }
    //------------------------------------------------------------------
    thread CgiHandler;
    try
    {
        CgiHandler = thread(cgi_handler, numChld);
    }
    catch (...)
    {
        print_err("%d<%s:%d> Error create thread(cgi_handler)\n", numChld, __func__, __LINE__);
        exit(1);
    }
    //------------------------------------------------------------------
    thread EventHandler;
    try
    {
        EventHandler = thread(event_handler, &ReqMan);
    }
    catch (...)
    {
        print_err("%d<%s:%d> Error create thread(send_file_)\n", numChld, __func__, __LINE__);
        exit(1);
    }
    //------------------------------------------------------------------
    int NumThr = 0;
    while (NumThr < conf->NumThreads)
    {
        thread thr;
        try
        {
            thr = thread(response1, &ReqMan);
        }
        catch (...)
        {
            print_err("%d<%s:%d> Error create thread\n", numChld, __func__, __LINE__);
            exit(1);
        }
        ++NumThr;
        thr.detach();
    }
    fprintf(stderr, "[%u] +++++ num threads=%u, pid=%u +++++\n", numChld, NumThr, getpid());
    //------------------------------------------------------------------
    unsigned char status = CONNECT_IGN;
    int run = 1;

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockServer, &readfds);

    while (run)
    {
        SOCKET clientSocket;
        struct sockaddr_storage clientAddr;
        socklen_t addrSize = sizeof(struct sockaddr_storage);

        if (status == CONNECT_IGN)
        {
    line_ = _pipe_;
            unsigned char ch;
            bool ret = ReadFile(pIn, &ch, sizeof(ch), NULL, NULL);
            if (!ret)
            {
                DWORD err = GetLastError();
                print_err("%d<%s:%d> Error ReadFile: %lu\n", numChld, __func__, __LINE__, err);
                break;
            }

            if (ch == PROC_CLOSE)
            {
                // Close next process
                WriteFile(pOut, &ch, sizeof(ch), NULL, NULL);
                break;
            }

            status = ch;
            continue;
        }
        else if (status == CONNECT_ALLOW)
        {
            if (is_maxconn())
            {//------ the number of connections is the maximum ---------
                // Allow connections next worker process
                bool ret = WriteFile(pOut, &status, sizeof(status), NULL, NULL);
                if (!ret)
                {
                    DWORD err = GetLastError();
                    print_err("%d<%s:%d> Error WriteFile: %lu\n", numChld, __func__, __LINE__, err);
                    break;
                }

                status = CONNECT_IGN;
                continue;
            }
        line_ = _sock_;
            FD_SET(sockServer, &readfds);
            int ret_sel = select(sockServer + 1, &readfds, NULL, NULL, NULL);
            if (ret_sel <= 0)
            {
                ErrorStrSock(__func__, numChld, "Error select()", WSAGetLastError());
                break;
            }

            if (!FD_ISSET(sockServer, &readfds))
                break;
            clientSocket = accept(sockServer, (struct sockaddr*)&clientAddr, &addrSize);
            if (clientSocket == INVALID_SOCKET)
            {
                int err = WSAGetLastError();
                ErrorStrSock(__func__, numChld, "Error accept()", err);
                if (err == WSAEMFILE)
                    continue;
                else
                    break;
            }

            Connect* req;
            req = create_req(numChld);
            if (!req)
            {
                shutdown(clientSocket, SD_BOTH);
                closesocket(clientSocket);
                continue;
            }

            u_long iMode = 1;
            if (ioctlsocket(clientSocket, FIONBIO, &iMode) == SOCKET_ERROR)
            {
                print_err("<%s:%d> Error ioctlsocket(): %d\n", __func__, __LINE__, WSAGetLastError());
            }

            req->init();
            req->numChld = numChld;
            req->numConn = ++allConn;
            req->numReq = 1;
            req->serverSocket = sockServer;
            req->clientSocket = clientSocket;
            req->timeout = conf->TimeOut;
            req->remoteAddr[0] = '\0';
            req->remotePort[0] = '\0';
            getnameinfo((struct sockaddr*)&clientAddr,
                addrSize,
                req->remoteAddr,
                sizeof(req->remoteAddr),
                req->remotePort,
                sizeof(req->remotePort),
                NI_NUMERICHOST | NI_NUMERICSERV);

            start_conn();
            push_pollin_list(req);
        }

        if (conf->BalancedLoad == 'y')
        {
            status = CONNECT_IGN;
            char ch = CONNECT_ALLOW;
            bool ret = WriteFile(pOut, &ch, sizeof(ch), NULL, NULL);
            if (!ret)
            {
                DWORD err = GetLastError();
                print_err("<%s:%d> Error WriteFile: %lu\n", __func__, __LINE__, err);
                break;
            }
        }
    }

    print_err("%d<%s:%d> allConn=%u\n", numChld, __func__, __LINE__, allConn);
    close_proc(sockServer, numChld);

    EventHandler.join();
    CgiHandler.join();

    print_err("%d<%s:%d> *** Exit  ***\n", numChld, __func__, __LINE__);
}
//======================================================================
Connect* create_req(int n_proc)
{
    Connect* req = new(nothrow) Connect;
    if (!req)
    {
        print_err("%d<%s:%d> Error malloc()\n", n_proc, __func__, __LINE__);
    }
    return req;
}
//======================================================================
void close_proc(SOCKET sockServer, int numChld)
{
    close_event_handler();
    close_cgi_handler();

    shutdown(sockServer, SD_BOTH);
    closesocket(sockServer);
    WSACleanup();
}
