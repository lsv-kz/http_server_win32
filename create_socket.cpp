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
