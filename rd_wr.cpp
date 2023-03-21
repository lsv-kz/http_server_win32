#include "main.h"
#include <winsock2.h>
using namespace std;

/*====================================================================*/
int wait_read(SOCKET sock, int timeout)
{
    WSAPOLLFD readfds;
    readfds.fd = sock;
    readfds.events = POLLIN;
    int ret, tm;

    if (timeout == -1)
        tm = -1;
    else
        tm = timeout * 1000;

    ret = WSAPoll(&readfds, 1, tm);
    if (ret == SOCKET_ERROR)
    {
        ErrorStrSock(__func__, __LINE__, "Error select()");
        return -1;
    }
    else if (!ret)
        return -RS408;

    if (readfds.revents & POLLIN)
        return 1;
    else if (readfds.revents & POLLHUP)
        return 0;
    else if (readfds.revents & POLLERR)
    {
        print_err("<%s:%d> POLLERR fdrd.revents = 0x%02x\n", __func__, __LINE__, readfds.revents);
        return -1;
    }

    print_err("<%s:%d> Error .revents = 0x%02x\n", __func__, __LINE__, readfds.revents);
    return -1;
}
//======================================================================
int read_timeout(SOCKET sock, char* buf, int len, int timeout)
{
    int read_bytes = 0, ret;
    char* p;

    p = buf;
    while (len > 0)
    {
        ret = wait_read(sock, timeout);
        if (ret < 0)
        {
            return ret;
        }
        else if (!ret)
            break;

        ret = recv(sock, p, len, 0);
        if (ret == -1)
        {
            ErrorStrSock(__func__, __LINE__, "Error recv()");
            return -1;
        }
        else if (ret == 0)
            break;
        else
        {
            p += ret;
            len -= ret;
            read_bytes += ret;
        }
    }

    return read_bytes;
}
//======================================================================
int write_timeout(SOCKET sock, const char* buf, size_t len, int timeout)
{
    WSAPOLLFD writefds;
    int ret, write_bytes = 0;
 //   print_err("<%s:%d> ------\n", __func__, __LINE__);
    writefds.fd = sock;
    writefds.events = POLLWRNORM;

    while (len > 0)
    {
        ret = WSAPoll(&writefds, 1, timeout * 1000);
        if (ret == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error WSAPoll()");
            return -1;
        }
        else if (!ret)
        {
            print_err("<%s:%d> TimeOut WSAPoll(), tm=%d\n", __func__, __LINE__, timeout);
            return -1;
        }

        if (writefds.revents != POLLWRNORM)
        {
            print_err("<%s:%d> writefds.revents=0x%x\n", __func__, __LINE__, writefds.revents);
            return -1;
        }

        ret = send(sock, buf, (int)len, 0);
        if (ret == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error send()");
            return -1;
        }

        write_bytes += ret;
        len -= ret;
        buf += ret;
    }

    return write_bytes;
}
//======================================================================
int ReadFromPipe(PIPENAMED* Pipe, char* buf, int sizeBuf, int* allRD, int maxRd, int timeout)
{
    DWORD dwRead = 0, ret = 1;
    bool bSuccess = false;
    char* p = buf;

    *allRD = 0;

    while (sizeBuf > 0)
    {
        DWORD rd = (sizeBuf < maxRd) ? sizeBuf : maxRd; //   &dwRead
        bSuccess = ReadFile(Pipe->parentPipe, p, rd, NULL, &Pipe->oOverlap);
        if (!bSuccess)
        {
            DWORD err = GetLastError();
            //	DWORD err = PrintError(__func__, __LINE__, "Error ReadFile()");
            if (err == ERROR_BROKEN_PIPE)     // 109
                return 0;
            else if (err == ERROR_IO_PENDING) // 997
            {
                DWORD dwWait = WaitForSingleObject(Pipe->hEvent, timeout * 1000);
                switch (dwWait)
                {
                case WAIT_OBJECT_0:
                    break;
                case WAIT_TIMEOUT:
                    print_err("<%s:%d> WAIT_TIMEOUT: %d s\n", __func__, __LINE__, timeout);
                    return -408;
                case WAIT_FAILED:
                    print_err("<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    return -1;
                default:
                    print_err("<%s%d> default: %lu\n", __func__, __LINE__, dwWait);
                    return -1;
                }
            }
            else
                return -1;
        }

        bSuccess = GetOverlappedResult(Pipe->parentPipe, &Pipe->oOverlap, &dwRead, false);
        if (!bSuccess)
        {
            DWORD err = GetLastError();
            //	DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
            if (err == ERROR_BROKEN_PIPE) // 109
            {
                return 0;
            }
            else if (err != ERROR_IO_INCOMPLETE) // 996
            {
                return -1;
            }
        }
        else
        {
            *allRD += dwRead;
            p += dwRead;
            sizeBuf -= dwRead;
        }
    }

    return ret;
}
//======================================================================
int WriteToPipe(PIPENAMED* Pipe, const char* buf, int lenBuf, int sizePipeBuf, int timeout)
{
    DWORD dwWrite = 0, allWR = 0;
    bool bSuccess = false;
    const char* p = buf;

    while (lenBuf > 0)
    {
        int wr = (lenBuf > sizePipeBuf) ? sizePipeBuf : lenBuf;// &dwWrite
        bSuccess = WriteFile(Pipe->parentPipe, p, wr, NULL, &Pipe->oOverlap);
        if (!bSuccess)
        {
            DWORD err = GetLastError();
            //	DWORD err = PrintError(__func__, __LINE__, "Error WriteFile()");
            if (err == ERROR_IO_PENDING)  //  997
            {
                DWORD dwWait = WaitForSingleObject(Pipe->hEvent, timeout * 1000);
                switch (dwWait)
                {
                case WAIT_OBJECT_0:
                    break;
                case WAIT_TIMEOUT:
                    print_err("<%s:%d> WAIT_TIMEOUT: %d s\n", __func__, __LINE__, timeout);
                    return -408;
                case WAIT_FAILED:
                    print_err("<%s:%d> WAIT_FAILED\n", __func__, __LINE__);
                    return -1;
                default:
                    print_err("<%s%d> default: %lu\n", __func__, __LINE__, dwWait);
                    return -1;
                }
            }
            else
                return -1;
        }

        bSuccess = GetOverlappedResult(Pipe->parentPipe, &Pipe->oOverlap, &dwWrite, false);
        if (!bSuccess)
        {
            DWORD err = GetLastError();
            //	DWORD err = PrintError(__func__, __LINE__, "Error GetOverlappedResult()");
            if (err == ERROR_BROKEN_PIPE) // 109
            {
                return -1;
            }
        }
        else
        {
            allWR += dwWrite;
            p += dwWrite;
            lenBuf -= dwWrite;
        }
    }

    return allWR;
}
//=====================================================================
long long client_to_script(SOCKET sock, PIPENAMED* Pipe, long long cont_len, int sizePipeBuf, int timeout)
{
    long long wr_bytes;
    int rd, n;
    DWORD wr;
    const int size_buf = 1024;
    char buf[size_buf];

    for (wr_bytes = 0; cont_len > 0; )
    {
        rd = wait_read(sock, conf->TimeOut);
        if (rd < 0)
        {
            return rd;
        }
        else if (!rd)
            break;

        if (cont_len > (long long)size_buf)
            n = (int)size_buf;
        else
            n = (int)cont_len;

        rd = recv(sock, buf, n, 0);
        if (rd == SOCKET_ERROR)
        {
            ErrorStrSock(__func__, __LINE__, "Error recv()");
            return -1;
        }
        else if (rd == 0)
            break;
        cont_len -= rd;

        wr = WriteToPipe(Pipe, buf, rd, sizePipeBuf, timeout);
        if (wr < 0)
        {
            print_err("<%s:%d>  Error WriteToPipe()=%d\n", __func__, __LINE__, wr);
            return -1;
        }

        wr_bytes += wr;
    }

    return wr_bytes;
}
//======================================================================
int send_file_1(SOCKET sock, int fd_in, char* buf, int* size, long long offset, long long* cont_len)
{
    int rd, wr, ret = 0;
    unsigned int n;

    _lseeki64(fd_in, offset, SEEK_SET);

    for (; *cont_len > 0; )
    {
        if (*cont_len < *size)
            n = (unsigned int)* cont_len;
        else
            n = *size;
        rd = _read(fd_in, buf, n);

        if (rd == -1)
        {
            print_err("<%s:%d> Error _read()\n", __func__, __LINE__);
            ret = rd;
            break;
        }
        else if (rd == 0)
        {
            print_err("<%s:%d> send_file() _read = 0 (EOF)\n", __func__, __LINE__);
            ret = rd;
            break;
        }

        wr = write_timeout(sock, buf, rd, conf->TimeOut);
        if (wr <= 0)
        {
            print_err("<%s:%d> Error write_to_sock()=%d\n", __func__, __LINE__, wr);
            ret = -1;
            break;
        }

        *cont_len -= wr;
    }

    return ret;
}
/*====================================================================*/
int send_file_2(SOCKET sock, int fd_in, char* buf, int size)
{
    int rd, wr;
    errno = 0;

    rd = _read(fd_in, buf, size);
    if (rd <= 0)
    {
        if (rd == -1)
            print_err("<%s:%d> Error _read(): errno=%d\n", __func__, __LINE__, errno);
        if (rd == 0)
            print_err("<%s:%d> Error _read()=0; %d\n", __func__, __LINE__, size);
        return rd;
    }

    wr = send(sock, buf, rd, 0);
    if (wr == SOCKET_ERROR)
    {
        ErrorStrSock(__func__, __LINE__, "Error send()");
        return -1;
    }

    if (rd != wr)
    {
        print_err("<%s:%d> %d != %d\n", __func__, __LINE__, rd, wr);
        _lseeki64(fd_in, (long long)wr - rd, SEEK_CUR);
    }

    return wr;
}
//======================================================================
int read_line_sock(SOCKET sock, char* buf, int size, int timeout)
{
    int ret, n, read_bytes = 0;

    for (; size > 0; )
    {
        n = wait_read(sock, timeout);
        if (n <= 0)
            return n;
        ret = recv(sock, buf, size, MSG_PEEK);
        if (ret > 0)
        {
            char* pr, * pn;
            pr = (char*)memchr(buf, '\r', ret);
            pn = (char*)memchr(buf, '\n', ret);
            if (pr && pn)
            {
                if ((pr + 1) == pn)
                {
                    n = (int)(pn - buf) + 1;
                    ret = recv(sock, buf, n, 0);
                    if (ret <= 0)
                    {
                        if (ret == SOCKET_ERROR)
                        {
                            ErrorStrSock(__func__, __LINE__, "Error recv()");
                        }
                        return ret;
                    }
                    return read_bytes + ret;
                }
                else if ((pr + 1) < pn)
                {
                    print_err("<%s:%d> Error: '\\n' not found\n", __func__, __LINE__);
                    return -RS400;
                }
                else if (pr > pn)
                {
                    print_err("<%s:%d> Error: '\\r' not found\n", __func__, __LINE__);
                    return -RS400;
                }
                else
                {
                    print_err("<%s:%d> Error: '?' not found\n", __func__, __LINE__);
                    return -RS400;
                }
            }
            else if ((!pr) && pn && (*(pn - 1) != '\r'))
            {
                print_err("<%s:%d> Error: '\\r' not found\n", __func__, __LINE__);
                return -RS400;
            }
            else if (pr && (!pn) && ((pr + 1) != (buf + ret)))
            {
                print_err("<%s:%d> Error: '\\n' not found\n", __func__, __LINE__);
                return -RS400;
            }

            n = recv(sock, buf, ret, 0);
            if (n != ret)
            {
                if (n == SOCKET_ERROR)
                {
                    ErrorStrSock(__func__, __LINE__, "Error recv()");
                }
                return -1;
            }
            buf += n;
            size -= n;
            read_bytes += n;
        }
        else // ret <= 0
        {
            if (ret == SOCKET_ERROR)
            {
                ErrorStrSock(__func__, __LINE__, "Error recv()");
            }
            return ret;
        }
    }

    return -RS414;
}
//======================================================================
int Connect::hd_read()
{
    if (err) return -1;
    int len = SIZE_BUF_REQUEST - req.len - 1;
    if (len <= 0)
        return -RS414;
    int n = recv(clientSocket, req.buf + req.len, len, 0);
    if (n <= 0)
        return -1;

    lenTail += n;
    req.len += n;
    req.buf[req.len] = 0;
    if (req.len > 0)
        timeout = conf->TimeOut;

    n = find_empty_line();
    if (n == 1)
        return req.len;
    else if (n < 0)
        return n;

    return 0;
}
//======================================================================
int Connect::find_empty_line()
{
    if (err) return -1;
    timeout = conf->TimeOut;
    char *pCR, *pLF;
    while (lenTail > 0)
    {
        int i = 0, len_line = 0;
        pCR = pLF = NULL;
        while (i < lenTail)
        {
            char ch = *(p_newline + i);
            if (ch == '\r')// found CR
            {
                if (i == (lenTail - 1))
                    return 0;
                if (pCR)
                    return -RS400;
                pCR = p_newline + i;
            }
            else if (ch == '\n')// found LF
            {
                pLF = p_newline + i;
                if ((pCR) && ((pLF - pCR) != 1))
                    return -RS400;
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

            if (len_line == 0) // found empty line
            {
                if (i_arrHdrs == 0) // empty lines before Starting Line
                {
                    if ((pLF - req.buf + 1) > 4) // more than two empty lines
                        return -RS400;
                    lenTail -= i;
                    p_newline = pLF + 1;
                    continue;
                }

                if (lenTail > 0) // tail after empty line (Message Body for POST method)
                {
                    tail = pLF + 1;
                    lenTail -= i;
                }
                else
                    tail = NULL;
                return 1;
            }

            if (i_arrHdrs < MAX_HEADERS)
            {
                arrHdrs[i_arrHdrs].ptr = p_newline;
                arrHdrs[i_arrHdrs].len = pLF - p_newline + 1;
                ++i_arrHdrs;
            }
            else
                return -RS500;

            lenTail -= i;
            p_newline = pLF + 1;
        }
        else if (pCR && (!pLF))
            return -RS400;
        else
            break;
    }

    return 0;
}