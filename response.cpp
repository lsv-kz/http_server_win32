#include "classes.h"

using namespace std;
//======================================================================
void response1(RequestManager* ReqMan)
{
    int n;
    const char* p;
    Connect* req;

    while (1)
    {
        req = ReqMan->pop_req();
        if (!req)
        {
            ReqMan->end_thr(1);
            return;
        }
        else if (req->clientSocket == INVALID_SOCKET)
        {
            ReqMan->end_thr(1);
            delete req;
            return;
        }
        /*-----------------------------------------------------*/
        int ret = parse_startline_request(req, req->arrHdrs[0].ptr, req->arrHdrs[0].len);
        if (ret)
        {
            print_err(req, "<%s:%d>  Error parse_startline_request(): %d\n", __func__, __LINE__, ret);
            goto end;
        }

        for (int i = 1; i < req->i_arrHdrs; ++i)
        {
            ret = parse_headers(req, req->arrHdrs[i].ptr, req->arrHdrs[i].len);
            if (ret < 0)
            {
                print_err(req, "<%s:%d>  Error parse_headers(): %d\n", __func__, __LINE__, ret);
                goto end;
            }
        }
        /*--------------------------------------------------------*/
        if ((req->httpProt != HTTP10) && (req->httpProt != HTTP11))
        {
            req->connKeepAlive = 0;
            req->err = -RS505;
            goto end;
        }

        if (req->numReq >= (unsigned int)conf->MaxRequestsPerClient || (req->httpProt == HTTP10))
        {
            print_err(req, "<%s:%d>  conf->MaxRequestsPerClient: %d\n", __func__, __LINE__, conf->MaxRequestsPerClient);
            req->connKeepAlive = 0;
        }
        else if (req->req_hdrs.iConnection == -1)
            req->connKeepAlive = 1;

        if ((p = strchr(req->uri, '?')))
        {
            req->uriLen = p - req->uri;
            req->sReqParam = req->uri + req->uriLen + 1;
        }
        else
        {
            req->sReqParam = NULL;
            req->uriLen = strlen(req->uri);
        }

        if (decode(req->uri, req->uriLen, req->decodeUri, sizeof(req->decodeUri)) <= 0)
        {
            print_err(req, "<%s:%d> Error: decode URI\n", __func__, __LINE__);
            req->err = -RS404;
            goto end;
        }

        clean_path(req->decodeUri);
        //--------------------------------------------------------------
        n = utf8_to_utf16(req->decodeUri, req->wDecodeUri);
        if (n)
        {
            print_err(req, "<%s:%d> utf8_to_utf16()=%d\n", __func__, __LINE__, n);
            req->err = -RS500;
            goto end;
        }
        //--------------------------------------------------------------
        if ((req->reqMethod == M_GET) || 
            (req->reqMethod == M_HEAD) || 
            (req->reqMethod == M_POST) || 
            (req->reqMethod == M_OPTIONS))
        {
            int ret = response2(ReqMan, req);
            if (ret == 1) // "req" may be free !!!
            {
                int ret = ReqMan->end_thr(0);
                if (ret == EXIT_THR)
                    return;
                else
                    continue;
            }

            req->err = ret;
        }
        else
            req->err = -RS501;

    end:
        end_response(req);

        ret = ReqMan->end_thr(0);
        if (ret == EXIT_THR)
        {
            return;
        }
    }
}
//======================================================================
const char boundary[] = "----------a9b5r7a4c0a2d5a1b8r3a";
int send_multypart(Connect* req, ArrayRanges& rg, char* rd_buf, int* size);
int create_multipart_head(Connect* req, Range* ranges, char* buf, int len_buf);
//======================================================================
long long file_size(const wchar_t* s)
{
    struct _stati64 st;

    if (!_wstati64(s, &st))
        return st.st_size;
    else
        return -1;
}
//======================================================================
int fastcgi(Connect* req, const wchar_t* wPath)
{
    const wchar_t* p = wcsrchr(wPath, '/');
    if (!p) return -RS404;
    fcgi_list_addr* i = conf->fcgi_list;
    for (; i; i = i->next)
    {
        if (i->scrpt_name[0] == L'~')
        {
            if (!wcscmp(p, i->scrpt_name.c_str() + 1))
                break;
        }
        else
        {
            if (i->scrpt_name == wPath)
                break;
        }
    }

    if (!i)
        return -RS404;
    req->resp.scriptType = FASTCGI;
    req->wScriptName = i->scrpt_name.c_str();
    push_cgi(req);
    return 1;
}
//======================================================================
int response2(RequestManager* ReqMan, Connect* req)
{
    if ((strstr(req->decodeUri, ".php")))
    {
        if ((conf->usePHP != "php-cgi") && (conf->usePHP != "php-fpm"))
        {
            print_err(req, "<%s:%d> Not found: %s\n", __func__, __LINE__, req->decodeUri);
            return -RS404;
        }
        struct _stat st;
        if (_wstat(req->wDecodeUri.c_str() + 1, &st) == -1)
        {
            print_err(req, "<%s:%d> script (%s) not found\n", __func__, __LINE__, req->decodeUri);
            return -RS404;
        }

        req->wScriptName = req->wDecodeUri;
        if (conf->usePHP == "php-cgi")
        {
            req->resp.scriptType = PHPCGI;
            push_cgi(req);
            return 1;
        }
        else if (conf->usePHP == "php-fpm")
        {
            req->resp.scriptType = PHPFPM;
            push_cgi(req);
            return 1;
        }
    }

    if (!strncmp(req->decodeUri, "/cgi-bin/", 9)
        || !strncmp(req->decodeUri, "/cgi/", 5))
    {
        req->wScriptName = req->wDecodeUri;
        req->resp.scriptType = CGI;
        push_cgi(req);
        return 1;
    }
    //-------------------------- get path ------------------------------
    wstring wPath;
    wPath.reserve(conf->wRootDir.size() + req->wDecodeUri.size() + 256);
    wPath += conf->wRootDir;
    wPath += req->wDecodeUri;
    if (wPath[wPath.size() - 1] == L'/')
        wPath.resize(wPath.size() - 1);
    //------------------------------------------------------------------
    struct _stati64 st64;
    if (_wstati64(wPath.c_str(), &st64) == -1)
    {
        int ret = fastcgi(req, req->wDecodeUri.c_str());
        if (ret < 0)
        {
            String sTmp;
            utf16_to_utf8(req->wDecodeUri, sTmp);
            print_err(req, "<%s:%d> Error not found (%d) [%s]\n", __func__, __LINE__, ret, sTmp.c_str());
        }
        return ret;
    }
    else
    {
        if ((!(st64.st_mode & _S_IFDIR)) && (!(st64.st_mode & _S_IFREG)))
        {
            print_err(req, "<%s:%d> Error: file (!S_ISDIR && !S_ISREG) \n", __func__, __LINE__);
            return -RS403;
        }
    }
    //------------------------------------------------------------------
    DWORD attr = GetFileAttributesW(wPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        PrintError(__func__, __LINE__, "GetFileAttributesW");
        return -RS500;
    }

    if (attr & FILE_ATTRIBUTE_HIDDEN)
    {
        print_err(req, "<%s:%d> Hidden\n", __func__, __LINE__);
        return -RS404;
    }

    if (st64.st_mode & _S_IFDIR)
    {
        if (req->reqMethod == M_POST)
            return -RS404;
        else if (req->reqMethod == M_OPTIONS)
        {
            req->resp.respContentType = "text/html; charset=utf-8";
            return options(req);
        }

        if (req->uri[req->uriLen - 1] != '/')
        {
            req->uri[req->uriLen] = '/';
            req->uri[req->uriLen + 1] = '\0';
            req->resp.respStatus = RS301;

            String hdrs(127);
            hdrs << "Location: " << req->uri << "\r\n";
            if (hdrs.error())
            {
                print_err(req, "<%s:%d> Error create_header()\n", __func__, __LINE__);
                return -RS500;
            }

            String s(256);
            s << "The document has moved <a href=\"" << req->uri << "\">here</a>";
            if (s.error())
            {
                print_err(req, "<%s:%d> Error create_header()\n", __func__, __LINE__);
                return -1;
            }

            return send_message(req, s.c_str());
        }
        //--------------------------------------------------------------
        size_t len = wPath.size();
        wPath += L"/index.html";
        struct _stat st;
        if ((_wstat(wPath.c_str(), &st) == -1) || (conf->index_html != 'y'))
        {
            wPath.resize(len);
            if ((conf->usePHP != "n") && (conf->index_php == 'y'))
            {
                wPath += L"/index.php";
                if (_wstat(wPath.c_str(), &st) == 0)
                {
                    int ret;
                    req->wScriptName = req->wDecodeUri + L"index.php";
                    if (conf->usePHP == "php-fpm")
                    {
                        //req->resp.scriptType = PHPFPM;
                        return -RS404;
                    }
                    else if (conf->usePHP == "php-cgi")
                    {
                        req->resp.scriptType = PHPCGI;
                        push_cgi(req);
                        return 1;
                    }
                    else
                        ret = -1;

                    req->wScriptName = L"";
                    return ret;
                }
                wPath.resize(len);
            }

            if (conf->index_pl == 'y')
            {
                req->resp.scriptType = CGI;
                req->wScriptName = L"/cgi-bin/index.pl";
                push_cgi(req);
                return 1;
            }
            else if (conf->index_fcgi == 'y')
            {
                /*req->resp.scriptType = FASTCGI;
                req->wScriptName = L"/index.fcgi";*/
                return -RS404;
            }

            return index_dir(req, wPath);
        }
    }

    if (req->reqMethod == M_POST)
        return -RS405;
    //-------------------------------------------------------------------
    req->resp.fileSize = file_size(wPath.c_str());
    req->resp.numPart = 0;
    req->resp.respContentType = content_type(wPath.c_str());
    if (req->reqMethod == M_OPTIONS)
        return options(req);

    ArrayRanges rg;
    if (req->req_hdrs.iRange >= 0)
    {
        try
        {
            req->resp.numPart = rg.create_ranges(req->sRange, req->resp.fileSize);
        }
        catch (...)
        {
            print_err(req, "<%s:%d> Error create_ranges\n", __func__, __LINE__);
            return -1;
        }
        
        req->resp.respStatus = RS206;
        if (req->resp.numPart == 1)
        {
            Range* pr = rg.get(0);
            if (pr)
            {
                req->resp.offset = pr->start;
                req->resp.respContentLength = pr->len;
                if (req->reqMethod == M_HEAD)
                {
                    if (send_response_headers(req) <= 0)
                        return -1;
                    return 0;
                }
            }
            else
                return -RS500;
        }
        else if (req->resp.numPart < 1)
            return -RS500;
    }
    else
    {
        req->resp.respStatus = RS200;
        req->resp.offset = 0;
        req->resp.respContentLength = req->resp.fileSize;
    }
    //------------------------------------------------------------------
    if (_wsopen_s(&req->resp.fd, wPath.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, _S_IREAD))
    {
        String sTmp;
        utf16_to_utf8(wPath, sTmp);
        print_err(req, "<%s:%d> Error _wopen(%s); err=%d\n", __func__, __LINE__, sTmp.c_str(), errno);
        if (errno == EACCES)
            return -RS403;
        else
            return -RS500;
    }

    wPath.clear();
    wPath.reserve(0);

    if (req->resp.numPart > 1)
    {
        int size = conf->SndBufSize;
        char* rd_buf = new(nothrow) char[size];
        if (!rd_buf)
        {
            print_err(req, "<%s:%d> Error malloc()\n", __func__, __LINE__);
            _close(req->resp.fd);
            return -1;
        }

        int ret = send_multypart(req, rg, rd_buf, &size);
        delete[] rd_buf;
        _close(req->resp.fd);
        return ret;
    }
    
    if (create_response_headers(req))
        return -1;

    push_send_file(req);
    return 1;
}
//======================================================================
int send_multypart(Connect* req, ArrayRanges& rg, char* rd_buf, int* size)
{
    int n;
    long long send_all_bytes = 0;
    char buf[1024];
    Range* range;
    
    for (int i = 0; (range = rg.get(i)) && (i < req->resp.numPart); ++i)
    {
        send_all_bytes += range->len;
        send_all_bytes += create_multipart_head(req, range, buf, sizeof(buf));
    }
    send_all_bytes += snprintf(buf, sizeof(buf), "\r\n--%s--\r\n", boundary);
    req->resp.respContentLength = send_all_bytes;

    String hdrs(256);
    hdrs << "Content-Type: multipart/byteranges; boundary=" << boundary << "\r\n";
    hdrs << "Content-Length: " << send_all_bytes << "\r\n";
    if (hdrs.error())
    {
        print_err(req, "<%s:%d> Error create response headers\n", __func__, __LINE__);
        return -1;
    }
    
    if (send_response_headers(req) <= 0)
        return -1;
    if (req->reqMethod == M_HEAD)
        return 0;

    send_all_bytes = 0;
    
    for (int i = 0; (range = rg.get(i)) && (i < req->resp.numPart); ++i)
    {
        if ((n = create_multipart_head(req, range, buf, sizeof(buf))) == 0)
        {
            print_err(req, "<%s:%d> Error create_multipart_head()=%d\n", __func__, __LINE__, n);
            return -1;
        }

        n = write_timeout(req->clientSocket, buf, strlen(buf), conf->TimeOut);
        if (n < 0)
        {
            print_err(req, "<%s:%d> Error: write_timeout(), %lld bytes\n",
                 __func__, __LINE__, send_all_bytes);
            return -1;
        }
        send_all_bytes += n;
        send_all_bytes += range->len;

        n = send_file_1(req->clientSocket, req->resp.fd, rd_buf, size, range->start, &range->len);
        if (n < 0)
        {
            print_err(req, "<%s:%d> Error: Sent %lld bytes\n", __func__, __LINE__, send_all_bytes);
            return -1;
        }
    }

    req->resp.send_bytes = send_all_bytes;
    snprintf(buf, sizeof(buf), "\r\n--%s--\r\n", boundary);
    n = write_timeout(req->clientSocket, buf, strlen(buf), conf->TimeOut);
    if (n < 0)
    {
        req->resp.send_bytes = send_all_bytes;
        print_err(req, "<%s:%d> Error: write_timeout() %lld\n", __func__, __LINE__, send_all_bytes);
        return -1;
    }
    req->resp.send_bytes += n;
    return 0;
}
/*====================================================================*/
int create_multipart_head(Connect* req, struct Range* ranges, char* buf, int len_buf)
{
    int n, all = 0;

    n = snprintf(buf, len_buf, "\r\n--%s\r\n", boundary);
    buf += n;
    len_buf -= n;
    all += n;

    if (req->resp.respContentType && (len_buf > 0))
    {
        n = snprintf(buf, len_buf, "Content-Type: %s\r\n", req->resp.respContentType);
        buf += n;
        len_buf -= n;
        all += n;
    }
    else
        return 0;

    if (len_buf > 0)
    {
        n = snprintf(buf, len_buf,
            "Content-Range: bytes %lld-%lld/%lld\r\n\r\n",
            ranges->start, ranges->end, req->resp.fileSize);
        buf += n;
        len_buf -= n;
        all += n;
    }
    else
        return 0;

    return all;
}
//======================================================================
int options(Connect* r)
{
    r->resp.respStatus = RS200;
    r->resp.respContentLength = 0;
    if (create_response_headers(r))
        return -1;

    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();
    r->html.len = 0;
    push_send_html(r);
    return 1;
}
