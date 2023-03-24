#include "main.h"

using namespace std;

static Config c;
const Config* const conf = &c;
//======================================================================
int check_path(wstring& path)
{
    wchar_t cwd[2048], new_cwd[2048];

    if (!(_wgetcwd(cwd, sizeof(cwd) / sizeof(wchar_t))))
        return -1;

    if (_wchdir(path.c_str()))
        return -1;

    if (!(_wgetcwd(new_cwd, sizeof(new_cwd) / sizeof(wchar_t))))
        return -1;

    if (new_cwd[wcslen(new_cwd) - 1] == '/')
        new_cwd[wcslen(new_cwd) - 1] = 0;
    path = new_cwd;

    if (_wchdir(cwd))
        return -1;

    return 0;
}
//======================================================================
static int line_ = 1, line_inc = 0;
//----------------------------------------------------------------------
int getLine(ifstream& fi, String& ss)
{
    ss = "";
    char ch;
    int len = 0, numWords = 0, wr = 1, wrSpace = 0;

    if (line_inc)
    {
        ++line_;
        line_inc = 0;
    }
    
    while (fi.get(ch))
    {
        if ((char)ch == '\n')
        {
            if (len)
            {
                line_inc = 1;
                return ++numWords;
            }
            else
            {
                ++line_;
                wr = 1;
                ss = "";
                wrSpace = 0;
                continue;
            }
        }

        if (wr == 0)
            continue;

        switch (ch)
        {
        case ' ':
        case '\t':
            if (len)
                wrSpace = 1;
        case '\r':
            break;
        case '#':
            wr = 0;
            break;
        case '{':
        case '}':
            if (len)
                fi.seekg(-1, ios::cur); // fi.putback(ch);
            else
            {
                ss << ch;
                ++len;
            }

            return ++numWords;
        default:
            if (wrSpace)
            {
                ss << " ";
                ++len;
                ++numWords;
                wrSpace = 0;
            }

            ss << ch;
            ++len;
        }
    }

    if (len)
        return ++numWords;
    return -1;
}
//======================================================================
int isnumber(const char* s)
{
    if (!s)
        return 0;
    int n = isdigit((int)*(s++));
    while (*s && n)
        n = isdigit((int)*(s++));
    return n;
}
//======================================================================
int isbool(const char* s)
{
    if (!s)
        return 0;
    if (strlen(s) != 1)
        return 0;
    return ((s[0] == 'y') || (s[0] == 'n'));
}
//======================================================================
int find_bracket(ifstream& fi)
{
    char ch;
    int grid = 0;

    if (line_inc)
    {
        ++line_;
        line_inc = 0;
    }
    
    while (fi.get(ch))
    {
        if (ch == '#')
            grid = 1;
        else if (ch == '\n')
        {
            grid = 0;
            ++line_;
        }
        else if ((ch == '{') && (grid == 0))
            return 1;
        else if ((ch != ' ') && (ch != '\t') && (grid == 0))
            return 0;
    }

    return 0;
}
//======================================================================
void create_fcgi_list(fcgi_list_addr** l, const String& s1, const String& s2)
{
    if (l == NULL)
        fprintf(stderr, "<%s:%d> Error pointer = NULL\n", __func__, __LINE__), exit(errno);

    fcgi_list_addr* t;
    try
    {
        t = new fcgi_list_addr;
    }
    catch (...)
    {
        fprintf(stderr, "<%s:%d> Error malloc()\n", __func__, __LINE__);
        exit(errno);
    }
    
    t->next = NULL;
    wstring stmp;
    utf8_to_utf16(s1.c_str(), stmp);
    t->scrpt_name = stmp;

    utf8_to_utf16(s2.c_str(), stmp);
    t->addr = stmp;

    t->next = *l;
    *l = t;
}
//======================================================================
int read_conf_file(const char* path_conf)
{
    String ss, nameFile;
    nameFile << path_conf;
    nameFile << "/server.conf";

    ifstream fconf(nameFile.c_str(), ios::binary);
    if (!fconf.is_open())
    {
        cerr << __func__ << "(): Error create conf file (" << nameFile.c_str() << ")\n";
        exit(1);
    }

    c.index_html = c.index_php = c.index_pl = c.index_fcgi = 'n';
    c.fcgi_list = NULL;

    int n;
    while ((n = getLine(fconf, ss)) > 0)
    {
        if (n == 2)
        {
            String s1, s2;
            ss >> s1;
            ss >> s2;

            if (s1 == "ServerAddr")
                s2 >> c.ServerAddr;
            else if (s1 == "Port")
                s2 >> c.ServerPort;
            else if (s1 == "ServerSoftware")
                s2 >> c.ServerSoftware;
            else if ((s1 == "SndBufSize") && isnumber(s2.c_str()))
                s2 >> c.SndBufSize;
            else if (s1 == "DocumentRoot")
            {
                String tmp;
                s2 >> tmp;
                utf8_to_utf16(tmp, c.wRootDir);
            }
            else if (s1 == "ScriptPath")
            {
                String stmp;
                s2 >> stmp;
                utf8_to_utf16(stmp, c.wCgiDir);
            }
            else if (s1 == "LogPath")
            {
                String stmp;
                s2 >> stmp;
                utf8_to_utf16(stmp, c.wLogDir);
            }
            else if ((s1 == "ListenBacklog") && isnumber(s2.c_str()))
                s2 >> c.ListenBacklog;
            else if ((s1 == "MaxRequests") && isnumber(s2.c_str()))
                s2 >> c.MaxRequests;
            else if ((s1 == "MaxEventSock") && isnumber(s2.c_str()))
                s2 >> c.MaxEventSock;
            else if ((s1 == "MaxRequestsPerClient") && isnumber(s2.c_str()))
                s2 >> c.MaxRequestsPerClient;
            else if ((s1 == "NumChld") && isnumber(s2.c_str()))
                s2 >> c.NumChld;
            else if ((s1 == "MaxThreads") && isnumber(s2.c_str()))
                s2 >> c.MaxThreads;
            else if ((s1 == "MinThreads") && isnumber(s2.c_str()))
                s2 >> c.MinThreads;
            else if ((s1 == "TimeoutKeepAlive") && isnumber(s2.c_str()))
                s2 >> c.TimeoutKeepAlive;
            else if ((s1 == "TimeOut") && isnumber(s2.c_str()))
                s2 >> c.TimeOut;
            else if ((s1 == "TimeOutCGI") && isnumber(s2.c_str()))
                s2 >> c.TimeOutCGI;
            else if ((s1 == "TimeoutPoll") && isnumber(s2.c_str()))
                s2 >> c.TimeoutPoll;
            else if (s1 == "PerlPath")
            {
                String stmp;
                s2 >> stmp;
                utf8_to_utf16(stmp, c.wPerlPath);
            }
            else if (s1 == "PyPath")
            {
                String stmp;
                s2 >> stmp;
                utf8_to_utf16(stmp, c.wPyPath);
            }
            else if (s1 == "PathPHP-CGI")
            {
                String stmp;
                s2 >> stmp;
                utf8_to_utf16(stmp, c.wPathPHP_CGI);
            }
            else if (s1 == "PathPHP-FPM")
                s2 >> c.pathPHP_FPM;
            else if (s1 == "UsePHP")
                s2 >> c.usePHP;
            else if ((s1 == "ShowMediaFiles") && isbool(s2.c_str()))
                c.ShowMediaFiles = s2[0];
            else if ((s1 == "ClientMaxBodySize") && isnumber(s2.c_str()))
                s2 >> c.ClientMaxBodySize;
            else
            {
                fprintf(stderr, "<%s:%d> Error read config file: [%s], line %d\n", __func__, __LINE__, ss.c_str(), line_);
                exit(1);
            }
        }
        else if (n == 1)
        {
            if (ss == "index")
            {
                if (find_bracket(fconf) == 0)
                {
                    fprintf(stderr, "<%s:%d> Error Error not found \"{\", line %d\n", __func__, __LINE__, line_);
                    exit(1);
                }

                while (getLine(fconf, ss) == 1)
                {
                    if (ss == "}")
                       break;
               
                    if (ss == "index.html")
                        c.index_html = 'y';
                    else if (ss == "index.php")
                        c.index_php = 'y';
                    else if (ss == "index.pl")
                        c.index_pl = 'y';
                    else if (ss == "index.fcgi")
                        c.index_fcgi = 'y';
                    else
                    {
                        printf("<%s:%d> Error read conf file(): \"index\" [%s], line %d\n", __func__, __LINE__, ss.c_str(), line_);
                        exit(1);
                    }
                }

                if (!(ss == "}"))
                {
                    fprintf(stderr, "<%s:%d> Error not found \"}\", line %d\n", __func__, __LINE__, line_);
                    exit(1);
                }
            }
            else if (ss == "fastcgi")
            {
                if (find_bracket(fconf) == 0)
                {
                    fprintf(stderr, "<%s:%d> Error Error not found \"{\", line %d\n", __func__, __LINE__, line_);
                    exit(1);
                }

                while (getLine(fconf, ss) == 2)
                {
                    String s1, s2;
                    ss >> s1;
                    ss >> s2;

                    create_fcgi_list(&c.fcgi_list, s1, s2);
                }

                if (ss.str() != "}")
                {
                    fprintf(stderr, "<%s:%d> Error not found \"}\", line %d\n", __func__, __LINE__, line_);
                    exit(1);
                }
            }
            else
            {
                fprintf(stderr, "<%s:%d> Error read config file: [%s], line %d\n", __func__, __LINE__, ss.c_str(), line_);
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, "<%s:%d> Error read config file: [%s], line %d\n", __func__, __LINE__, ss.c_str(), line_);
            exit(1);
        }
    }

    fconf.close();
    /*
    fcgi_list_addr* i = c.fcgi_list;
    for (; i; i = i->next)
    {
        wcerr << L"   [" << i->scrpt_name.c_str() << L"] = [" << i->addr.c_str() << L"]\n";
    }*/
    //-------------------------log_dir--------------------------------------
    if (check_path(c.wLogDir) == -1)
    {
        wcerr << L" Error log_dir: " << c.wLogDir << L"\n";
        cin.get();
        exit(1);
    }
    path_correct(c.wLogDir);
    //------------------------------------------------------------------
    if (check_path(c.wRootDir) == -1)
    {
        wcerr << L"!!! Error root_dir: " << c.wRootDir << L"\n";
        exit(1);
    }

    path_correct(c.wRootDir);
    if (c.wRootDir[c.wRootDir.size() - 1] == L'/')
        c.wRootDir.resize(c.wRootDir.size() - 1);
    //------------------------------------------------------------------
    if (check_path(c.wCgiDir) == -1)
    {
        wcerr << L"!!! Error cgi_dir: " << c.wCgiDir << L"\n";
        exit(1);
    }
    path_correct(c.wCgiDir);
    if (c.wCgiDir[c.wCgiDir.size() - 1] == L'/')
        c.wCgiDir.resize(c.wCgiDir.size() - 1);

    return 0;
}
