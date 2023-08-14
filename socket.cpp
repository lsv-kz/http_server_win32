#include "main.h"

//======================================================================
int in4_aton(const char* host, struct in_addr* addr)
{
    char ch[4];
    int i = 0;
    const char* p = host;
    while (i < 4)
    {
        if (*p == 0)
            break;
        ch[i] = (char)strtol((char*)p, (char**)& p, 10);
        *((char*)addr + i) = ch[i];
        ++i;
        if (*p++ != '.')
            break;
    }

    return i;
}
//======================================================================
SOCKET create_server_socket(const Config * conf)
{
    SOCKADDR_IN sin;
    unsigned short port;
    WSADATA wsaData = { 0 };

    port = (unsigned short)atol(conf->ServerPort.c_str());
    if (WSAStartup(MAKEWORD(2, 2), &wsaData))
    {
        print_err("<%s:%d> Error WSAStartup(): %d\n", __func__, __LINE__, WSAGetLastError());
        system("PAUSE");
        return INVALID_SOCKET;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = PF_INET;

    //  sin.sin_addr.s_addr = INADDR_ANY;
    if (in4_aton(conf->ServerAddr.c_str(), &(sin.sin_addr)) != 4)
    {
        print_err("<%s:%d> Error in4_aton()\n", __func__, __LINE__);
        getchar();
        return INVALID_SOCKET;
    }
    sin.sin_port = htons(port);

    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET)
    {
        print_err("<%s:%d> Error socket(): %d\n", __func__, __LINE__, WSAGetLastError());
        WSACleanup();
        getchar();
        return INVALID_SOCKET;
    }

    int sock_opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char*)& sock_opt, sizeof(sock_opt)) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error setsockopt(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        getchar();
        return INVALID_SOCKET;
    }

    sock_opt = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)& sock_opt, sizeof(sock_opt)) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error setsockopt(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        getchar();
        return INVALID_SOCKET;
    }

    if (bind(sockfd, (SOCKADDR*)(&sin), sizeof(sin)) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error bind(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        getchar();
        return INVALID_SOCKET;
    }

    if (listen(sockfd, conf->ListenBacklog) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error listen(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        getchar();
        return INVALID_SOCKET;
    }

    return sockfd;
}
//======================================================================
int send_file(SOCKET sock, int fd_in, char* buf, int size)
{
    int rd, wr;
    if (size <= 0)
        return -1;
    rd = _read(fd_in, buf, size);
    if (rd <= 0)
    {
        if (rd == -1)
            print_err("<%s:%d> Error _read(): errno=%d\n", __func__, __LINE__, errno);
        if (rd == 0)
            print_err("<%s:%d> Error _read()=0; %d\n", __func__, __LINE__, size);
        return -1;
    }

    wr = send(sock, buf, rd, 0);
    if (wr == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            _lseeki64(fd_in, -rd, SEEK_CUR);
            return TRYAGAIN;
        }
        return -1;
    }

    if (rd != wr)
    {
        print_err("<%s:%d> %d != %d\n", __func__, __LINE__, rd, wr);
        _lseeki64(fd_in, wr - rd, SEEK_CUR);
    }

    return wr;
}
//======================================================================
int Connect::read_request_headers()
{
    if (err) return -1;
    int len = SIZE_BUF_REQUEST - req.len - 1;
    if (len <= 0)
        return -RS414;
    int n = recv(clientSocket, req.buf + req.len, len, 0);
    if (n == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return TRYAGAIN;
        return -1;
    }
    else if (n == 0)
        return -1;
    lenTail += n;
    req.len += n;
    req.buf[req.len] = 0;
    if (req.len > 0)
        timeout = conf->TimeOut;

    n = find_empty_line();
    if (n == 1)
    {
        return req.len;
    }
    else if (n < 0)
    {
        print_err(this, "<%s:%d> ok\n", __func__, __LINE__);
        return n;
    }
print_err(this, "<%s:%d> ok\n", __func__, __LINE__);
    return 0;
}
//======================================================================
SOCKET create_fcgi_socket(Connect *r, const char* host)
{
    char addr[256];
    char port[16] = "";
    std::string sHost = host;

    if (!host)
    {
        print_err("<%s:%d> Error: host=NULL\n", __func__, __LINE__);
        return INVALID_SOCKET;
    }

    size_t sz = sHost.find(':');
    if (sz == std::string::npos)
    {
        print_err("<%s:%d> \n", __func__, __LINE__);
        return INVALID_SOCKET;
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
        ErrorStrSock(__func__, __LINE__, "Error socket()", WSAGetLastError());
        return INVALID_SOCKET;
    }

    sock_addr.sin_port = htons(atoi(port));
    sock_addr.sin_family = AF_INET;

    if (in4_aton(addr, &(sock_addr.sin_addr)) != 4)
    {
        print_err("<%s:%d> Error in4_aton()=%d\n", __func__, __LINE__);
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    DWORD sock_opt = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)& sock_opt, sizeof(sock_opt)) == SOCKET_ERROR)
    {
        print_err("<%s:%d> Error setsockopt(): %d\n", __func__, __LINE__, WSAGetLastError());
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    u_long iMode = 1;
    if (ioctlsocket(sockfd, FIONBIO, &iMode) == SOCKET_ERROR)
    {
        ErrorStrSock(__func__, __LINE__, "Error ioctlsocket()", WSAGetLastError());
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    if (connect(sockfd, (struct sockaddr*)(&sock_addr), sizeof(sock_addr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) // err != 10035
        {
            print_err("<%s:%d> Error connect(): %d\n", __func__, __LINE__, err);
            closesocket(sockfd);
            return INVALID_SOCKET;
        }

        r->io_status = SELECT;
    }
    else
        r->io_status = WORK;

    return sockfd;
}
//======================================================================
int write_to_fcgi(Connect* r)
{
    int ret = send(r->fcgi.fd, r->cgi.p, r->cgi.len_buf, 0);
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)   // 10035
            return TRYAGAIN;
        else if (err == WSAENOTCONN) // 10057
        {
            if (r->scriptType == SCGI)
            {
                if (r->cgi.status.scgi == SCGI_PARAMS)
                    return TRYAGAIN;
            }
            else
            {
                if (r->cgi.status.fcgi == FASTCGI_BEGIN)
                    return TRYAGAIN;
            }
        }

        ErrorStrSock(__func__, __LINE__, "Error send()", err);
        return -1;
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
            return -1;
        }
        else if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return TRYAGAIN;
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
