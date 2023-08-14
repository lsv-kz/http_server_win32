#include "main.h"

using namespace std;

struct stFile
{
    string name;
    long long size = 0LL;
};

int create_index_html(Connect* req, vector <string>& vecDirs, vector <struct stFile>& vecFiles);
string encode(const string& s_in);
//======================================================================
static int isimage(const char* name)
{
    const char* p;

    p = strrchr(name, '.');
    if (!p)
        return 0;

    if (!strlcmp_case(p, (char*)".gif", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".png", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".ico", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".svg", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".jpeg", 5) || !strlcmp_case(p, (char*)".jpg", 4)) return 1;
    return 0;
}
//======================================================================
static int isaudiofile(const char* name)
{
    const char* p;

    if (!(p = strrchr(name, '.'))) return 0;

    if (!strlcmp_case(p, (char*)".wav", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".mp3", 4)) return 1;
    else if (!strlcmp_case(p, (char*)".ogg", 4)) return 1;
    return 0;
}
//======================================================================
bool compareVec(stFile& s1, stFile& s2)
{
    return (s1.name < s2.name);
}
//======================================================================
int index_dir(Connect* r, wstring & path)
{
    int dirs, files;
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = INVALID_HANDLE_VALUE;

    vector <string> vecDirs;
    vector <struct stFile> vecFiles;

    path += L"/*";
    hFind = FindFirstFileW(path.c_str(), &ffd);
    if (INVALID_HANDLE_VALUE == hFind)
    {
        String str;
        utf16_to_utf8(path, str);
        print_err(r, "<%s:%d> Error FindFirstFileW(\"%s\")\n", __func__, __LINE__, str.c_str());
        return -RS500;
    }

    dirs = files = 0;
    do
    {
        if (ffd.cFileName[0] == '.') continue;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ||
            (ffd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) ||
            (ffd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ||
            (ffd.dwFileAttributes > FILE_ATTRIBUTE_NORMAL))
        {
            continue;
        }

        string fname;
        int err = utf16_to_utf8(ffd.cFileName, fname);
        if (err == 0)
        {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (dirs > 255) continue;
                vecDirs.push_back(fname);
                dirs++;
            }
            else
            {
                if (files > 255) continue;
                stFile tmp;
                tmp.name = fname;
                tmp.size = ((ffd.nFileSizeHigh * (MAXDWORD + 1LL)) + ffd.nFileSizeLow);
                vecFiles.push_back(tmp);
                files++;
            }
        }
    } while (FindNextFileW(hFind, &ffd) != 0);
    FindClose(hFind);

    sort(vecDirs.begin(), vecDirs.end());
    sort(vecFiles.begin(), vecFiles.end(), compareVec);

    int ret = create_index_html(r, vecDirs, vecFiles);
    if (ret >= 0)
    {
        r->html.p = r->html.s.c_str();
        r->html.len = r->html.s.size();

        r->resp.respStatus = RS200;
        r->mode_send = NO_CHUNK;
        if (create_response_headers(r))
        {
            print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
            r->err = -1;
            return -1;
        }

        r->resp_headers.p = r->resp_headers.s.c_str();
        r->resp_headers.len = r->resp_headers.s.size();
        push_send_html(r);
        return 1;
    }

    return ret;
}
//======================================================================
int create_index_html(Connect* r, vector <string> & vecDirs, vector <struct stFile> & vecFiles)
{
    int dirs = vecDirs.size(), files = vecFiles.size();
    if (r->reqMethod == M_HEAD)
        return 0;
    r->html.s = "";

    r->html.s << "<!DOCTYPE HTML>\n"
            "<html>\n"
            " <head>\n"
            "  <meta charset=\"UTF-8\">\n"
            "  <title>Index of " << r->decodeUri << "</title>\n"
            "  <style>\n"
            "    body {\n"
            "     margin-left:100px; margin-right:50px;\n"
            "    }\n"
            "  </style>\n"
            "  <link href=\"/styles.css\" type=\"text/css\" rel=\"stylesheet\">"
            " </head>\n"
            " <body id=\"top\">\n"
            "  <h3>Index of " << r->decodeUri << "</h3>\n"
            "  <table cols=\"2\" width=\"100\x25\">\n"
            "   <tr><td><h3>Directories</h3></td><td></td></tr>\n";
    //------------------------------------------------------------------
    if (!strcmp(r->decodeUri, "/"))
        r->html.s << "   <tr><td></td><td></td></tr>\n";
    else
        r->html.s << "   <tr><td><a href=\"../\">Parent Directory/</a></td><td></td></tr>\n";
    //-------------------------- Directories ---------------------------
    for (int i = 0; i < dirs; ++i)
    {
        r->html.s << "   <tr><td><a href=\"" << encode(vecDirs[i]) << "/\">" << vecDirs[i] << "/</a></td>"
                "<td align=right></td></tr>\n";
    }
    //------------------------------------------------------------------
    r->html.s << "   <tr><td><hr></td><td><hr></td></tr>\n"
            "   <tr><td><h3>Files</h3></td><td></td></tr>\n";
    //---------------------------- Files -------------------------------
    for (int i = 0; i < files; ++i)
    {
        if (isimage(vecFiles[i].name.c_str()) && (conf->ShowMediaFiles == 'y'))
        {
            if (vecFiles[i].size < 20000)
                r->html.s << "   <tr><td><a href=\"" << encode(vecFiles[i].name) << "\"><img src=\"" <<
                encode(vecFiles[i].name) << "\"></a><br>";
            else
                r->html.s << "   <tr><td><a href=\"" << encode(vecFiles[i].name) << "\"><img src=\"" << encode(vecFiles[i].name) <<
                "\" width=\"320\"></a><br>";
            r->html.s << vecFiles[i].name << "</td><td align=\"right\">" << vecFiles[i].size << " bytes</td></tr>\n"
                "   <tr><td></td><td></td></tr>\n";
        }
        else if (isaudiofile(vecFiles[i].name.c_str()) && (conf->ShowMediaFiles == 'y'))
            r->html.s << "   <tr><td><audio preload=\"none\" controls src=\"" << encode(vecFiles[i].name) << "\"></audio>"
            "<a href=\"" << encode(vecFiles[i].name) << "\">" << vecFiles[i].name <<
            "</a></td><td align=\"right\">" << vecFiles[i].size << " bytes</td></tr>\n";
        else
            r->html.s << "   <tr><td><a href=\"" << encode(vecFiles[i].name) << "\">" << vecFiles[i].name << "</a></td>"
            "<td align=\"right\">" << vecFiles[i].size << " bytes</td></tr>\n";
    }
    //------------------------------------------------------------------
    r->html.s << "  </table>\n"
            "  <hr>\n"
            "  " << r->resp.sTime << "\n"
            "  <a href=\"#top\" style=\"display:block;\n"
            "         position:fixed;\n"
            "         bottom:30px;\n"
            "         left:10px;\n"
            "         width:50px;\n"
            "         height:40px;\n"
            "         font-size:60px;\n"
            "         background:gray;\n"
            "         border-radius:10px;\n"
            "         color:black;\n"
            "         opacity: 0.7\">^</a>\n"
            " </body>\n"
            "</html>";
    //------------------------------------------------------------------
    r->resp.respContentLength = r->html.s.size();
    r->resp.respContentType = "text/html";

    return (int)r->resp.respContentType;
}

