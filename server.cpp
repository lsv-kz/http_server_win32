#include "main.h"
#include <sstream>

using namespace std;

static SOCKET sockServer = -1;
int read_conf_file(const char* path_conf);
//======================================================================
int main_proc(const char* name_proc);
void child_proc(SOCKET sock, int numChld, HANDLE);
//======================================================================
BOOL WINAPI sigHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        printf("signal: Ctrl-C\n");
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
    if (argc == 8)
    {
        setlocale(LC_CTYPE, "");
        if (!strcmp(argv[1], "child"))
        {
            int numChld;
            DWORD ParentID;
            SOCKET sockServ;
            HANDLE hReady;
            HANDLE hChildLog, hChildLogErr;

            stringstream ss;
            ss << argv[2] << ' ' << argv[3] << ' '
                << argv[4] << ' ' << argv[5] << ' '
                << argv[6] << ' ' << argv[7];
            ss >> numChld;
            ss >> ParentID;
            ss >> sockServ;
            ss >> hReady;
            ss >> hChildLog;
            ss >> hChildLogErr;

            open_logfiles(hChildLog, hChildLogErr);
            child_proc(sockServ, numChld, hReady);
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
        << "\n   pid = " << pid
        << "\n   ip = " << conf->ServerAddr.c_str()
        << "\n   port = " << conf->ServerPort.c_str()
        << "\n   SndBufSize = " << conf->SndBufSize
        << "\n\n   NumChld = " << conf->NumChld
        << "\n\n   ListenBacklog = " << conf->ListenBacklog
        << "\n   MaxRequests = " << conf->MaxRequests
        << "\n\n   MaxRequestsPerClient " << conf->MaxRequestsPerClient
        << "\n   TimeoutKeepAlive = " << conf->TimeoutKeepAlive
        << "\n   TimeOut = " << conf->TimeOut
        << "\n   TimeoutPoll = " << conf->TimeoutPoll
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
    HANDLE hExit_in = NULL;
    HANDLE hExit_out = NULL;

    if (!CreatePipe(&hExit_in, &hExit_out, &saAttr, 0))
    {
        cerr << "<" << __LINE__ << "> Error: CreatePipe" << "\n";
        cin.get();
        exit(1);
    }

    if (!SetHandleInformation(hExit_in, HANDLE_FLAG_INHERIT, 0))
    {
        cerr << "<" << __LINE__ << "> Error: SetHandleInformation" << "\n";
        cin.get();
        exit(1);
    }
    //------------------------------------------------------------------
    int numChld = 0;
    while (numChld < conf->NumChld)
    {
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
           << sockServer << ' '<< hExit_out << ' ' << hLog << ' ' << hLogErr;

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
        ++numChld;
    }

    CloseHandle(hExit_out);
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
    while (1)
    {
        //unsigned char ch;
        DWORD rd, pid;
        bool res = ReadFile(hExit_in, &pid, sizeof(pid), &rd, NULL);
        if (!res || rd == 0)
        {
            int err = GetLastError();
            print_err("<%s:%d> Error ReadFile(): %d\n", __func__, __LINE__, err);
            break;
        }
        print_err("<%s:%d> *** Child process [%d] closed ***\n", __func__, __LINE__, (int)pid);
        printf("<%s:%d> *** Child process [%d] closed ***\n", __func__, __LINE__, (int)pid);
    }

    CloseHandle(hExit_in);

    print_err("<%s:%d> Close main_proc\n", __func__, __LINE__);

    return 0;
}
