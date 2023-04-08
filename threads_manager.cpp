#include "main.h"

using namespace std;

Connect* create_req(RequestManager* ReqMan);

static mutex mtx_conn;
static condition_variable cond_close_conn;

static int count_conn = 0;
//======================================================================
RequestManager::RequestManager(int n)
{
    list_start = list_end = NULL;
    size_list = stop_manager = all_thr = 0;
    count_thr = num_wait_thr = 0;
    numChld = n;
}
//----------------------------------------------------------------------
int RequestManager::get_num_chld(void)
{
    return numChld;
}
//----------------------------------------------------------------------
int RequestManager::get_num_thr(void)
{
    lock_guard<std::mutex> lg(mtx_thr);
    return count_thr;
}
//----------------------------------------------------------------------
int RequestManager::start_thr(void)
{
    mtx_thr.lock();
    int ret = ++count_thr;
    ++all_thr;
    mtx_thr.unlock();
    return ret;
}
//----------------------------------------------------------------------
void RequestManager::wait_exit_thr(int n)
{
    unique_lock<mutex> lk(mtx_thr);
    while (n == count_thr)
    {
        cond_exit_thr.wait(lk);
    }
}
//----------------------------------------------------------------------
void push_resp_list(Connect* req, RequestManager* ReqMan)
{
    ReqMan->mtx_thr.lock();
    req->next = NULL;
    req->prev = ReqMan->list_end;
    if (ReqMan->list_start)
    {
        ReqMan->list_end->next = req;
        ReqMan->list_end = req;
    }
    else
        ReqMan->list_start = ReqMan->list_end = req;

    ++ReqMan->size_list;
    ReqMan->mtx_thr.unlock();
    ReqMan->cond_list.notify_one();
}
//----------------------------------------------------------------------
Connect* RequestManager::pop_req()
{
    unique_lock<mutex> lk(mtx_thr);
    ++num_wait_thr;

    while (list_start == NULL)
    {
        cond_list.wait(lk);
    }
    --num_wait_thr;
    Connect* req = list_start;
    if (!list_start) return NULL;

    if (list_start->next)
    {
        list_start->next->prev = NULL;
        list_start = list_start->next;
    }
    else
        list_start = list_end = NULL;

    --size_list;
    if (num_wait_thr <= 1)
        cond_new_thr.notify_one();

    return req;
}
//----------------------------------------------------------------------
int RequestManager::end_thr(int ret)
{
    mtx_thr.lock();
    if (((count_thr > conf->MinThreads) && (size_list <= num_wait_thr)) || ret)
    {
        --count_thr;
        ret = EXIT_THR;
    }

    mtx_thr.unlock();
    if (ret)
    {
        cond_exit_thr.notify_all();
        cond_new_thr.notify_all();
    }

    return ret;
}
//----------------------------------------------------------------------
int RequestManager::wait_create_thr(int* n)
{
    unique_lock<mutex> lk(mtx_thr);
    while (((num_wait_thr >= 1) || (count_thr >= conf->MaxThreads) || !count_conn) && !stop_manager)
    {
        cond_new_thr.wait(lk);
    }

    *n = count_thr;
    return stop_manager;
}
//--------------------------------------------
void RequestManager::close_manager()
{
    stop_manager = 1;
    cond_new_thr.notify_one();
    cond_exit_thr.notify_one();
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
static void check_num_conn()
{
    unique_lock<mutex> lk(mtx_conn);
    while (count_conn >= conf->MaxRequests)
        cond_close_conn.wait(lk);
}
//======================================================================
void thr_create_manager(int numProc, RequestManager* ReqMan)
{
    int num_thr;
    thread thr;

    while (1)
    {
        if (ReqMan->wait_create_thr(&num_thr))
            break;

        try
        {
            thr = thread(response1, ReqMan);
        }
        catch (...)
        {
            print_err("%d<%s:%d> Error create thread: num_thr=%d, errno=%d\n", numProc, __func__, __LINE__, num_thr);
            ReqMan->wait_exit_thr(num_thr);
            continue;
        }

        thr.detach();

        ReqMan->start_thr();
    }
    //print_err("%d<%s:%d> Exit thread_req_manager()\n", numProc, __func__, __LINE__);
}
//======================================================================
BOOL WINAPI childSigHandler(DWORD signal)
{

    if (signal == CTRL_C_EVENT)
    {
        print_err("<%s> signal: Ctrl-C\n", __func__);
        WSACleanup();
    }

    return TRUE;
}
//======================================================================
void child_proc(SOCKET sockServer, int numChld, HANDLE hExit_out)
{
    int n;
    int allNumThr = 0;
    unsigned long allConn = 0;
    RequestManager* ReqMan;

    setbuf(stderr, NULL);

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
    ReqMan = new(nothrow) RequestManager(numChld);
    if (!ReqMan)
    {
        print_err("<%s:%d> *********** Exit child %d ***********\n", __func__, __LINE__, numChld);
        exit(1);
    }
    //------------------------------------------------------------------
    thread CgiHandler;
    try
    {
        CgiHandler = thread(cgi_handler, ReqMan);
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
        EventHandler = thread(event_handler, ReqMan);
    }
    catch (...)
    {
        print_err("%d<%s:%d> Error create thread(send_file_)\n", numChld, __func__, __LINE__);
        exit(1);
    }
    //------------------------------------------------------------------
    n = 0;
    while (n < conf->MinThreads)
    {
        thread thr;
        try
        {
            thr = thread(response1, ReqMan);
        }
        catch (...)
        {
            print_err("%d<%s:%d> Error create thread\n", numChld, __func__, __LINE__);
            exit(1);
        }
        ++allNumThr;
        ReqMan->start_thr();
        thr.detach();
        ++n;
    }
    //------------------------------------------------------------------
    thread thrReqMan;
    try
    {
        thrReqMan = thread(thr_create_manager, numChld, ReqMan);
    }
    catch (...)
    {
        print_err("<%s:%d> Error create thread %d\n", __func__, __LINE__, allNumThr);
        exit(1);
    }

    fprintf(stderr, "[%u] +++++ num threads=%u, pid=%u +++++\n", numChld, ReqMan->get_num_thr(), getpid());

    while (1)
    {
        SOCKET clientSocket;
        socklen_t addrSize;
        struct sockaddr_storage clientAddr;

        check_num_conn();

        addrSize = sizeof(struct sockaddr_storage);
        clientSocket = accept(sockServer, (struct sockaddr*)&clientAddr, &addrSize);
        if (clientSocket == INVALID_SOCKET)
        {
            int err = ErrorStrSock(__func__, __LINE__, "Error accept()");
            if (err == WSAEMFILE)
            {
                continue;
            }
            else
            {
                break;
            }
        }

        Connect* req;
        req = create_req(ReqMan);
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

    shutdown(sockServer, SD_BOTH);
    closesocket(sockServer);

    n = ReqMan->get_num_thr();
    print_err("%d<%s:%d>  numThr=%d; allConn=%u\n", numChld,
        __func__, __LINE__, n, allConn);

    ReqMan->close_manager();
    close_event_handler();
    close_cgi_handler();
    
    thrReqMan.join();
    EventHandler.join();
    CgiHandler.join();

    DWORD rd, pid = GetCurrentProcessId();
    bool res = WriteFile(hExit_out, &pid, sizeof(pid), &rd, NULL);
    if (!res)
    {
        PrintError(__func__, __LINE__, "Error WriteFile()");
    }
    CloseHandle(hExit_out);

    print_err("%d<%s:%d> *** Exit  ***\n", numChld, __func__, __LINE__);
    delete ReqMan;
    WSACleanup();
}
//======================================================================
Connect* create_req(RequestManager* ReqMan)
{
    Connect* req = new(nothrow) Connect;
    if (!req)
    {
        print_err("%d<%s:%d> Error malloc()\n", ReqMan->get_num_chld(), __func__, __LINE__);
    }
    return req;
}
