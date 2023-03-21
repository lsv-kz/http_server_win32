#include "classes.h"

using namespace std;

int cgi_chunk(Connect* req, PIPENAMED* Pipe, int maxRd);
//======================================================================
class CreateEnv
{
private:
    size_t sizeBufEnv;
    size_t i;
    char* bufEnv;

public:
    size_t add(const char* name, const char* val)
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
        if ((i + len + 2) > sizeBufEnv)
        {
            int sizeTmp = sizeBufEnv + len + 128;
            char* tmp = new(nothrow) char[sizeTmp];
            if (!tmp)
                return 0;
            sizeBufEnv = sizeTmp;
            memcpy(tmp, bufEnv, i);
            delete[] bufEnv;
            bufEnv = tmp;
        }
        memcpy(bufEnv + i, ss.c_str(), len);
        i += len;
        bufEnv[i++] = '\0';
        bufEnv[i] = '\0';
        return len;
    }
    //------------------------------------------------------------------
    char* get() { return bufEnv; }
    //------------------------------------------------------------------
    size_t size() { return i + 1; }
    //------------------------------------------------------------------
    CreateEnv()
    {
        i = 0;
        sizeBufEnv = 1024;
        bufEnv = new(nothrow) char[sizeBufEnv];
        if (!bufEnv)
            sizeBufEnv = 0;
    }
    //------------------------------------------------------------------
    ~CreateEnv()
    {
        if (bufEnv) delete[] bufEnv;
    }
};
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
int cgi(Connect* req)
{
    int retExit = -1;
    size_t len;
    long long wr_bytes;
    struct _stat st;
    BOOL bSuccess;
    String stmp;
// print_err(req, "<%s:%d> --------\n", __func__, __LINE__);
    char pipeName[40] = "\\\\.\\pipe\\cgi";
    const DWORD PIPE_BUFSIZE = 1024;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE childPipe;
    PIPENAMED Pipe;
    ZeroMemory(&Pipe.oOverlap, sizeof(Pipe.oOverlap));

    wstring commandLine;
    CreateEnv env;

    wstring wPath;

    if (req->resp.scriptType == cgi_ex)
    {
        wPath += conf->wCgiDir;
        wPath += cgi_script_file(req->wScriptName);
    }
    else if (req->resp.scriptType == php_cgi)
    {
        wPath += conf->wRootDir;
        wPath += req->wScriptName;
    }

    if (_wstat(wPath.c_str(), &st) == -1)
    {
        utf16_to_utf8(wPath, stmp);
        print_err(req, "<%s:%d> script (%s) not found: errno=%d\n", __func__,
            __LINE__, stmp.c_str(), errno);
        return -RS404;
    }
    //--------------------- set environment ----------------------------
     {
        const int size = 4096;
        char tmpBuf[size];
        if (GetWindowsDirectory(tmpBuf, size))
        {
            env.add("SYSTEMROOT", tmpBuf);
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
            env.add("PATH", tmpBuf);
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
            env.add("CONTENT_LENGTH", req->req_hdrs.Value[req->req_hdrs.iReqContentLength]);
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
            env.add("CONTENT_TYPE", req->req_hdrs.Value[req->req_hdrs.iReqContentType]);
        else
        {
            print_err(req, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }
    }

    if (req->resp.scriptType == php_cgi)
        env.add("REDIRECT_STATUS", "true");
    if (req->req_hdrs.iUserAgent >= 0)
        env.add("HTTP_USER_AGENT", req->req_hdrs.Value[req->req_hdrs.iUserAgent]);
    if (req->req_hdrs.iReferer >= 0)
        env.add("HTTP_REFERER", req->req_hdrs.Value[req->req_hdrs.iReferer]);
    if (req->req_hdrs.iHost >= 0)
        env.add("HTTP_HOST", req->req_hdrs.Value[req->req_hdrs.iHost]);

    env.add("SERVER_PORT", conf->ServerPort.c_str());

    env.add("SERVER_SOFTWARE", conf->ServerSoftware.c_str());
    env.add("GATEWAY_INTERFACE", "CGI/1.1");

    if (req->reqMethod == M_HEAD)
        env.add("REQUEST_METHOD", get_str_method(M_GET));
    else
        env.add("REQUEST_METHOD", get_str_method(req->reqMethod));
    
    env.add("REMOTE_HOST", req->remoteAddr);
    env.add("SERVER_PROTOCOL", get_str_http_prot(req->httpProt));

    utf16_to_utf8(conf->wRootDir, stmp);
    env.add("DOCUMENT_ROOT", stmp.c_str());

    env.add("REQUEST_URI", req->uri);

    utf16_to_utf8(wPath, stmp);
    env.add("SCRIPT_FILENAME", stmp.c_str());

    //utf16_to_utf8(req->wDecodeUri, stmp);
    //env.add("SCRIPT_NAME", stmp.c_str());
    env.add("SCRIPT_NAME", req->decodeUri);
    
    env.add("GGGG_GGGG", "фы ва");

    env.add("REMOTE_ADDR", req->remoteAddr);
    env.add("REMOTE_PORT", req->remotePort);
    env.add("QUERY_STRING", req->sReqParam);
    //------------------------------------------------------------------
    if (req->resp.scriptType == php_cgi)
    {
        commandLine = conf->wPathPHP_CGI;
    }
    else if (req->resp.scriptType == cgi_ex)
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
    Pipe.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (Pipe.hEvent == NULL)
    {
        print_err(req, "<%s:%d> CreateEvent failed with %lu\n", __func__, __LINE__, GetLastError());
        return -RS500;
    }
    Pipe.oOverlap.hEvent = Pipe.hEvent;
    //------------------------------------------------------------------

    len = strlen(pipeName);
    snprintf(pipeName + len, sizeof(pipeName) - len, "%d%d", req->numChld, req->numConn);
    Pipe.parentPipe = CreateNamedPipeA(
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
    if (Pipe.parentPipe == INVALID_HANDLE_VALUE)
    {
        print_err(req, "<%s:%d> CreateNamedPipe failed, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(Pipe.hEvent);
        return -RS500;
    }

    if (!SetHandleInformation(Pipe.parentPipe, HANDLE_FLAG_INHERIT, 0))
    {
        print_err(req, "<%s:%d> Error SetHandleInformation, GLE=%lu\n", __func__, __LINE__, GetLastError());
        CloseHandle(Pipe.hEvent);
        DisconnectNamedPipe(Pipe.parentPipe);
        CloseHandle(Pipe.parentPipe);
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
        CloseHandle(Pipe.hEvent);
        DisconnectNamedPipe(Pipe.parentPipe);
        CloseHandle(Pipe.parentPipe);
        return -RS500;
    }

    ConnectNamedPipe(Pipe.parentPipe, &Pipe.oOverlap);

    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));

    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = childPipe;
    si.hStdInput = childPipe;
    si.hStdError = GetHandleLogErr();
    si.dwFlags |= STARTF_USESTDHANDLES;

    bSuccess = CreateProcessW(NULL, (wchar_t*)commandLine.c_str(), NULL, NULL, TRUE, 0, env.get(), NULL, &si, &pi);
    CloseHandle(childPipe);
    if (!bSuccess)
    {
        utf16_to_utf8(commandLine, stmp);
        print_err(req, "<%s:%d> Error CreateProcessW(%s)\n", __func__, __LINE__, stmp.c_str());
        PrintError(__func__, __LINE__, "Error CreateProcessW()");
        CloseHandle(Pipe.hEvent);
        DisconnectNamedPipe(Pipe.parentPipe);
        CloseHandle(Pipe.parentPipe);
        return -RS500;
    }
    else
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    //------------ write to script ------------
    if (req->reqMethod == M_POST)
    {
        if (req->tail)
        {
            wr_bytes = WriteToPipe(&Pipe, req->tail, req->lenTail, PIPE_BUFSIZE, conf->TimeOutCGI);
            if (wr_bytes < 0)
            {
                print_err(req, "%d<%s:%d> Error write_to_script()=%d\n", __func__, __LINE__, wr_bytes);
                CloseHandle(Pipe.hEvent);
                DisconnectNamedPipe(Pipe.parentPipe);
                CloseHandle(Pipe.parentPipe);
                return -RS500;
            }
            req->req_hdrs.reqContentLength -= wr_bytes;
        }

        if (req->req_hdrs.reqContentLength > 0)
        {
            wr_bytes = client_to_script(req->clientSocket, &Pipe, req->req_hdrs.reqContentLength, PIPE_BUFSIZE, conf->TimeOutCGI);
            if (wr_bytes < 0)
            {
                print_err(req, "<%s:%d> Error client_to_script()\n", __func__, __LINE__);
                CloseHandle(Pipe.hEvent);
                DisconnectNamedPipe(Pipe.parentPipe);
                CloseHandle(Pipe.parentPipe);
                return -RS500;
            }
            else
                req->req_hdrs.reqContentLength -= wr_bytes;
        }
    }

    retExit = cgi_chunk(req, &Pipe, PIPE_BUFSIZE);

    DisconnectNamedPipe(Pipe.parentPipe);
    CloseHandle(Pipe.parentPipe);
    CloseHandle(Pipe.hEvent);
    return retExit;
}
//======================================================================
int cgi_chunk(Connect* req, PIPENAMED* Pipe, int maxRd)
{
    int n, ReadFromScript = 0;
    char buf[256], * start_ptr;
    bool broken_pipe = false;
    int chunk_mode;
    if (req->reqMethod == M_HEAD)
        chunk_mode = NO_SEND;
    else
    {
        if ((req->httpProt == HTTP11) && req->connKeepAlive)
            chunk_mode = SEND_CHUNK;
        else
            chunk_mode = SEND_NO_CHUNK;
    }

    ClChunked chunk(req->clientSocket, chunk_mode);
    //------------ read from script ------------
    req->resp.respStatus = RS200;
    n = ReadFromPipe(Pipe, buf, sizeof(buf) - 1, &ReadFromScript, maxRd, conf->TimeOutCGI);
    if (n < 0)
    {
        print_err(req, "%d<%s:%d> Error script_to_buf()=%d\n", __func__, __LINE__, ReadFromScript);
        if (n == -RS408)
            return -RS500;
        return -1;
    }

    if (n == 0)
        broken_pipe = true;
    buf[ReadFromScript] = 0;
    //-------------------create headers of response---------------------
    start_ptr = buf;
    for (int line = 0; line < 10; ++line)
    {
        int len = 0;
        char *end_ptr, *str, *p;

        str = end_ptr = start_ptr;
        for ( ; ReadFromScript > 0; end_ptr++)
        {
            ReadFromScript--;
            if (*end_ptr == '\r')
                *end_ptr = 0;
            else if (*end_ptr == '\n')
                break;
            else
                len++;
        }

        if (*end_ptr == '\n')
            *end_ptr = 0;
        else
        {
            print_err(req, "<%s:%d> Error\n", __func__, __LINE__);
            return -1;
        }

        start_ptr = end_ptr + 1;

        if (len == 0)
            break;
   //     print_err(req, "<%s:%d> %s\n", __func__, __LINE__, str);
        if (!(p = (char*)memchr(str, ':', len)))
        {
            print_err(req, "<%s:%d> Error: Line not header [%s]\n", __func__, __LINE__, str);
            return -1;
        }
        
        if (!strlcmp_case(str, "Status", 6))
        {
            req->resp.respStatus = atoi(p + 1);
            /*if (req->resp.respStatus == 0)
                return -1;
            if (req->resp.respStatus == RS204)
            {
                return send_message(req, NULL);
            }*/
            continue;
        }

        if (!strlcmp_case(str, "Date", 4) || \
            !strlcmp_case(str, "Server", 6) || \
            !strlcmp_case(str, "Accept-Ranges", 13) || \
            !strlcmp_case(str, "Content-Length", 14) || \
            !strlcmp_case(str, "Connection", 10))
        {
            continue;
        }

        req->hdrs << str << "\r\n";
    }
print_err("%s\n", req->hdrs.c_str());
    //------------------------------------------------------------------
    req->resp.numPart = 0;
    req->resp.respContentType = NULL;
    req->resp.respContentLength = -1;

    if (req->reqMethod == M_HEAD)
    {
        req->resp.respContentLength = -1;

        if (send_response_headers(req) <= 0)
        {
            print_err("%d<%s:%d> Error send_header_response()\n", req->numChld, __func__, __LINE__);
            return -1;
        }
        return 0;
    }


    if (chunk_mode == SEND_CHUNK)
    {
        req->hdrs << "Transfer-Encoding: chunked\r\n";
    }

    if (send_response_headers(req) <= 0)
    {
        return -1;
    }

    if (req->resp.respStatus == RS204)
    {
        return 0;
    }
    //------------------ send entity to client -------------------------
    if (ReadFromScript > 0)
    {
        int n = chunk.add_arr(start_ptr, ReadFromScript);
        if (n < 0)
        {
            print_err(req, "<%s:%d> Error chunk.add_arr(): %d\n", __func__, __LINE__, n);
            return -1;
        }
    }

    if (!broken_pipe)
    {
        ReadFromScript = chunk.cgi_to_client(Pipe, maxRd);
        if (ReadFromScript < 0)
        {
            print_err(req, "<%s:%d> Error chunk.cgi_to_client()=%d\n", __func__, __LINE__, ReadFromScript);
            return -1;
        }
    }

    ReadFromScript = chunk.end();
    req->resp.send_bytes = chunk.all();
    if (ReadFromScript < 0)
    {
        print_err(req, "<%s:%d> Error chunk.end(): %d\n", __func__, __LINE__, ReadFromScript);
        return -1;
    }

    return 0;
}
