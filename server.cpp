#include "main.h"
#include <sstream>

using namespace std;
//======================================================================
static SOCKET sockServer = -1;
HANDLE pIn[PROC_LIMIT + 1], pfd_in = NULL;
HANDLE pOut[PROC_LIMIT + 1];
//======================================================================
int read_conf_file(const char* path_conf);
int main_proc(const char* name_proc);
void child_proc(SOCKET sock, int numChld, HANDLE, HANDLE);
//======================================================================
BOOL WINAPI sigHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        printf("<%s> signal: Ctrl-C\n", __func__);
    }

    return TRUE;
}
//======================================================================
int main(int argc, char* argv[])
{
    if (read_conf_file(".") < 0)
    {
        cin.get();
        exit(1);
    }
    //------------------------------------------------------------------
    if (argc == 9)
    {
        setlocale(LC_CTYPE, "");
        if (!strcmp(argv[1], "child"))
        {
            int numChld;
            DWORD ParentID;
            SOCKET sockServ;
            HANDLE hChildLog, hChildLogErr;
            HANDLE hIn, hOut;

            stringstream ss;
            ss << argv[2] << ' ' << argv[3] << ' '
                << argv[4] << ' ' << argv[5] << ' '
                << argv[6] << ' ' << argv[7] << ' '
                << argv[8];
            ss >> numChld;
            ss >> ParentID;
            ss >> sockServ;
            ss >> hChildLog;
            ss >> hChildLogErr;
            ss >> hIn;
            ss >> hOut;

            set_logfiles(hChildLog, hChildLogErr);
            child_proc(sockServ, numChld, hIn, hOut);
            exit(0);
        }
        else
        {
            exit(1);
        }
    }
    else if  (argc == 1)
    {
        printf(" LC_CTYPE: %s\n", setlocale(LC_CTYPE, ""));

        if (!SetConsoleCtrlHandler(sigHandler, TRUE))
        {
            printf("\nERROR: Could not set control handler\n");
            return 1;
        }
        main_proc(argv[0]);
        exit(0);
    }
    cout << "<" << __LINE__ << "> Error [argc=" << argc << "]\n";
    return 0;
}
//======================================================================
void create_logfiles(const wchar_t* log_dir, HANDLE* h, HANDLE* hErr);
//======================================================================
int main_proc(const char* name_proc)
{
    DWORD pid = GetCurrentProcessId();
    HANDLE hLog, hLogErr;
    create_logfiles(conf->wLogDir.c_str(), &hLog, &hLogErr);

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = true;
    saAttr.lpSecurityDescriptor = NULL;
    //------------------------------------------------------------------
    sockServer = create_server_socket(conf);
    if (sockServer == INVALID_SOCKET)
    {
        cout << "<" << __LINE__ << "> server: failed to bind" << "\n";
        cin.get();
        exit(1);
    }

    if (conf->NumChld < 1)
    {
        cerr << "<" << __LINE__ << "> Error NumChld = " << conf->NumChld << "; [NumChld < 1]\n";
        cin.get();
        exit(1);
    }
    cerr << " [" << get_time().c_str() << "] - server \"" << conf->ServerSoftware.c_str() << "\" run\n"
        << "\n   hardware_concurrency = " << thread::hardware_concurrency()
        << "\n   pid = " << pid
        << "\n   ip = " << conf->ServerAddr.c_str()
        << "\n   port = " << conf->ServerPort.c_str()
        << "\n   SndBufSize = " << conf->SndBufSize
        << "\n\n   NumChld = " << conf->NumChld
        << "\n   NumThreads = " << conf->NumThreads
        << "\n\n   ListenBacklog = " << conf->ListenBacklog
        << "\n   MaxWorkConnections = " << conf->MaxWorkConnections
        << "\n\n   MaxRequestsPerClient " << conf->MaxRequestsPerClient
        << "\n   TimeoutKeepAlive = " << conf->TimeoutKeepAlive
        << "\n   TimeOut = " << conf->TimeOut
        << "\n   TimeoutSel = " << conf->TimeoutSel
        << "\n   TimeoutCGI = " << conf->TimeoutCGI
        << "\n\n   php: " << conf->usePHP.c_str()
        << "\n\n   path_php-fpm: " << conf->pathPHP_FPM.c_str();
    wcerr << L"\n   path_php-cgi: " << conf->wPathPHP_CGI
        << L"\n   pyPath: " << conf->wPyPath
        << L"\n   PerlPath: " << conf->wPerlPath
        << L"\n   root_dir = " << conf->wRootDir
        << L"\n   cgi_dir = " << conf->wCgiDir
        << L"\n   log_dir = " << conf->wLogDir
        << L"\n   ShowMediaFiles = " << conf->ShowMediaFiles
        << L"\n   ClientMaxBodySize = " << conf->ClientMaxBodySize
        << L"\n\n";
    //------------------------------------------------------------------
    if (!CreatePipe(pIn, pOut, &saAttr, 0))
    {
        cerr << "<" << __LINE__ << "> Error: CreatePipe" << "\n";
        cin.get();
        exit(1);
    }
    
    pfd_in = pIn[0];

    int numChld = 0;
    while (numChld < conf->NumChld)
    {
        if (!CreatePipe(&pIn[numChld + 1], &pOut[numChld + 1], &saAttr, 0))
        {
            cerr << "<" << __LINE__ << "> Error: CreatePipe" << "\n";
            cin.get();
            exit(1);
        }
        
        pfd_in = pIn[numChld + 1];
        
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(STARTUPINFO));
        si.cb = sizeof(STARTUPINFO);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        //si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdError = GetHandleLogErr();

        String ss;
        ss << name_proc << " child " << numChld << ' ' << pid << ' '
           << sockServer << ' ' << hLog << ' ' << hLogErr << ' ' 
           << pIn[numChld] << ' ' << pOut[numChld + 1];

        bool bSuccess = CreateProcessA(NULL, (char*)ss.c_str(), NULL, NULL, true, 0, NULL, NULL, &si, &pi);
        if (!bSuccess)
        {
            DWORD err = GetLastError();
            print_err("<%s:%d> Error CreateProcessA(): %s\n error=%lu\n", __func__, __LINE__, ss.c_str(), err);
            exit(1);
        }

        cout << "[" << numChld << "] ProcessId: " << pi.dwProcessId << "\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        CloseHandle(pIn[numChld]);
        CloseHandle(pOut[numChld + 1]);
        ++numChld;
    }
    //------------------------------------------------------------------
    if (_wchdir(conf->wRootDir.c_str()))
    {
        wcerr << "!!! Error chdir(\"" << conf->wRootDir << "\") : " << errno << "\n";
        cin.get();
        exit(1);
    }
    //------------------------------------------------------------------
    shutdown(sockServer, SD_BOTH);
    closesocket(sockServer);
    WSACleanup();
    
    unsigned char status = CONNECT_ALLOW;
    bool ret = WriteFile(pOut[0], &status, sizeof(status), NULL, NULL);
    if (!ret)
    {
        DWORD err = GetLastError();
        print_err("<%s:%d> Error WriteFile: %lu\n", __func__, __LINE__, err);
        print_err("<%s:%d> Close main_proc\n", __func__, __LINE__);
        return 1;
    }
    
    while (1)
    {
        bool ret = ReadFile(pfd_in, &status, sizeof(status), NULL, NULL);
        if (!ret)
        {
            DWORD err = GetLastError();
            print_err("<%s:%d> Error ReadFile: %lu\n", __func__, __LINE__, err);
            break;
        }

        if (status == PROC_CLOSE)
        {
            ret = WriteFile(pOut[0], &status, sizeof(status), NULL, NULL);
            if (!ret)
            {
                DWORD err = GetLastError();
                print_err("<%s:%d> Error WriteFile: %lu\n", __func__, __LINE__, err);
            }

            break;
        }
        else if (status == CONNECT_ALLOW)
        {
            ret = WriteFile(pOut[0], &status, sizeof(status), NULL, NULL);
            if (!ret)
            {
                DWORD err = GetLastError();
                print_err("<%s:%d> Error WriteFile: %lu\n", __func__, __LINE__, err);
                break;
            }
        }
        else
            print_err("<%s:%d> !!! status: 0x%x\n", __func__, __LINE__, (int)status);
    }

    CloseHandle(pOut[0]);
    CloseHandle(pfd_in);

    print_err("<%s:%d> ***** Close main_proc *****\n", __func__, __LINE__);

    return 0;
}
