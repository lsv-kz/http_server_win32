#ifndef SERVER_H_
#define SERVER_H_
#define _WIN32_WINNT  0x0601

#include <iostream>
#include <fstream>
#include <algorithm>
#include <vector>

#include <mutex>
#include <thread>
#include <condition_variable>

#include <stdio.h>
#include <cstdlib>
#include <string.h>
#include <cassert>
#include <climits>
#include <wchar.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#include <io.h>
#include <sys/types.h>
#include <share.h>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#include <Winsock2.h>
#include <winsock.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <process.h>

#include "String.h"

const int MAX_NAME = 256;
const int SIZE_BUF_REQUEST = 8192;
const int MAX_HEADERS = 25;
const int BUF_TMP_SIZE = 10000;

enum {
    RS101 = 101,
    RS200 = 200, RS204 = 204, RS206 = 206,
    RS301 = 301, RS302,
    RS400 = 400, RS401, RS402, RS403, RS404, RS405, RS406, RS407,
    RS408, RS411 = 411, RS413 = 413, RS414, RS415, RS416, RS417, RS418,
    RS500 = 500, RS501, RS502, RS503, RS504, RS505
};

enum {
    M_GET = 1, M_HEAD, M_POST, M_OPTIONS, M_PUT,
    M_PATCH, M_DELETE, M_TRACE, M_CONNECT
};

enum { HTTP09 = 1, HTTP10, HTTP11, HTTP2 };

enum { EXIT_THR = 1 };

enum MODE_SEND { NO_CHUNK, CHUNK, CHUNK_END };
enum SOURCE_ENTITY { ENTITY_NONE, FROM_FILE, FROM_DATA_BUFFER, };
enum OPERATION_TYPE { READ_REQUEST = 1, SEND_RESP_HEADERS, SEND_ENTITY, DYN_PAGE, };
enum POLL_STATUS { WAIT = 1, WORK };

enum CGI_TYPE { CGI_TYPE_NONE, CGI, PHPCGI, PHPFPM, FASTCGI, SCGI, };
enum DIRECT { FROM_CGI = 1, TO_CGI, FROM_CLIENT, TO_CLIENT };

enum CGI_STATUS  { CGI_CREATE_PROC = 1, CGI_STDIN, CGI_READ_HTTP_HEADERS, CGI_SEND_HTTP_HEADERS, CGI_SEND_ENTITY };

enum FCGI_STATUS { FASTCGI_CONNECT = 1, FASTCGI_BEGIN, FASTCGI_PARAMS, FASTCGI_STDIN,
               FASTCGI_READ_HEADER, FASTCGI_READ_HTTP_HEADERS, FASTCGI_SEND_HTTP_HEADERS, FASTCGI_SEND_ENTITY,  
               FASTCGI_READ_ERROR, FASTCGI_READ_PADDING, FASTCGI_CLOSE };

enum SCGI_STATUS { SCGI_CONNECT = 1, SCGI_PARAMS, SCGI_STDIN, SCGI_READ_HTTP_HEADERS, SCGI_SEND_HTTP_HEADERS, SCGI_SEND_ENTITY, };

typedef struct fcgi_list_addr {
    std::wstring script_name;
    std::wstring addr;
    int type;
    struct fcgi_list_addr* next = NULL;
} fcgi_list_addr;

struct Param
{
    String name;
    String val;
};

void print_err(const char* format, ...);
//----------------------------------------------------------------------
typedef struct
{
    OVERLAPPED oOverlap;
    HANDLE parentPipe;
    HANDLE hEvent;
    DWORD dwState;
    BOOL fPendingIO;
    bool timeout;
} PIPENAMED;

struct Config
{
    std::string ServerSoftware = "?";
    std::string ServerAddr = "0.0.0.0";
    std::string ServerPort = "8080";

    int SndBufSize = 16284;

    int NumChld = 1;
    int MaxThreads = 18;
    int MinThreads = 6;
    unsigned int MaxCgiProc = 10;

    int ListenBacklog = 128;
    int MaxRequests = 512;

    int MaxRequestsPerClient = 50;
    int TimeoutKeepAlive = 5;
    int TimeOut = 30;
    int TimeoutCGI = 5;
    int TimeoutPoll = 100;

    std::wstring wLogDir = L"";
    std::wstring wRootDir = L"";
    std::wstring wCgiDir = L"";
    std::wstring wPerlPath = L"";
    std::wstring wPyPath = L"";

    std::string usePHP = "n";
    std::wstring wPathPHP_CGI = L"";
    std::string pathPHP_FPM = "";

    long int ClientMaxBodySize = 1000000;

    char index_html = 'n';
    char index_php = 'n';
    char index_pl = 'n';
    char index_fcgi = 'n';

    char ShowMediaFiles = 'n';

    fcgi_list_addr* fcgi_list = NULL;
    ~Config()
    {
        fcgi_list_addr* t;
        while (fcgi_list)
        {
            t = fcgi_list;
            fcgi_list = fcgi_list->next;
            if (t)
                delete t;
        }
    }
};

extern const Config* const conf;
//---------------------------------------------------------
struct hdr {
    char* ptr;
    int len;
};

union STATUS { CGI_STATUS cgi; FCGI_STATUS fcgi; SCGI_STATUS scgi; };
struct Cgi
{
    PIPENAMED Pipe;

    char *bufEnv;
    size_t sizeBufEnv;
    size_t lenEnv;
        
    STATUS status;
    DIRECT dir;
    char buf[8 + 4096 + 8];
    int  size_buf = 4096;
    long len_buf;
    long len_post;
    char *p;
        
    Cgi();
    int init(size_t);
    size_t param(const char* name, const char* val);
        
    ~Cgi();
};

class Connect
{
public:
    Connect* prev;
    Connect* next;

    SOCKET serverSocket;

    unsigned int numChld, numReq, numConn;

    SOCKET    clientSocket;
    int       err;
    __time64_t sock_timer;
    int       timeout;
    short     event;
    OPERATION_TYPE operation;
    POLL_STATUS    poll_status;

    char remoteAddr[NI_MAXHOST];
    char remotePort[NI_MAXSERV];

    struct
    {
        char buf[SIZE_BUF_REQUEST];
        int  len;
    } req;
    
    char* p_newline;
    char* tail;
    int   lenTail;

    int  i_arrHdrs;
    hdr  arrHdrs[MAX_HEADERS + 1];

    char  decodeUri[SIZE_BUF_REQUEST];

    std::wstring  wDecodeUri;

    char* uri;
    size_t uriLen;

    int  reqMethod;
    
    std::wstring wScriptName;
    
    const char* sReqParam;
    char* sRange;
    int   httpProt;
    int   connKeepAlive;

    struct
    {
        int       iConnection;
        int       iHost;
        int       iUserAgent;
        int       iReferer;
        int       iUpgrade;
        int       iReqContentType;
        int       iReqContentLength;
        int       iAcceptEncoding;
        int       iRange;
        int       iIf_Range;

        int       countReqHeaders;
        long long reqContentLength;

        const char* Name[MAX_HEADERS + 1];
        const char* Value[MAX_HEADERS + 1];
    } req_hdrs;
    /*--------------------------*/
    struct
    {
        String s;
        const char* p;
        int len;
    } resp_headers;

    String hdrs;

    struct
    {
        String s;
        const char* p;
        int len;
    } html;

    Cgi cgi;

    struct
    {
        bool http_headers_received;
        int fd;

        int i_param;
        int size_par;
        std::vector <Param> vPar;

        unsigned char fcgi_type;
        int dataLen;
        int paddingLen;
        char *ptr_header;
        int len_header;
    } fcgi;
    
    SOURCE_ENTITY source_entity;
    MODE_SEND mode_send;
    
    struct {
        int  respStatus;
        std::string sTime;
        long long respContentLength;
        const char *respContentType;
        long long fileSize;
        int  countRespHeaders = 0;

        CGI_TYPE scriptType;

        int  numPart;
        int  fd;
        long long offset;
        long long send_bytes;
    } resp;
    //----------------------------------------
    void init()
    {
        err = 0;
        decodeUri[0] = '\0';
        sRange = NULL;
        sReqParam = NULL;

        //------------------------------------
        uri = NULL;
        p_newline = req.buf;
        tail = NULL;
        //------------------------------------
        lenTail = 0;
        req.len = 0;
        i_arrHdrs = 0;
        reqMethod = 0;
        httpProt = 0;
        connKeepAlive = 0;

        req_hdrs = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, -1LL };
        req_hdrs.Name[0] = NULL;
        req_hdrs.Value[0] = NULL;

        resp.respStatus = 0;
        resp.fd = -1;
        resp.fileSize = 0;
        resp.offset = 0;
        resp.respContentLength = -1LL;
        resp.numPart = 0;
        resp.send_bytes = 0LL;
        resp.respContentType = NULL;
        resp.scriptType = CGI_TYPE_NONE;
        resp.countRespHeaders = 0;
        resp.sTime = "";
        hdrs = "";
    }

    int hd_read();
    int find_empty_line();
};

class RequestManager
{
private:
    Connect* list_start;
    Connect* list_end;

    std::mutex  mtx_thr;

    std::condition_variable cond_list;
    std::condition_variable cond_new_thr, cond_exit_thr;

    int num_wait_thr, size_list;
    int numChld, count_thr, stop_manager;

    unsigned long all_thr;
public:
    RequestManager(const RequestManager&) = delete;
    RequestManager(int);
    //-------------------------------
    int get_num_chld(void);
    int get_num_thr(void);
    int start_thr(void);
    int end_thr(int);
    void wait_exit_thr(int n);
    friend void push_resp_list(Connect* req, RequestManager*);
    Connect* pop_req();

    int wait_create_thr(int* n);
    void close_manager();
};

//======================================================================
int in4_aton(const char* host, struct in_addr* addr);
SOCKET create_server_socket(const Config* conf);

void response1(RequestManager* ReqMan);
int response2(RequestManager* ReqMan, Connect* req);
int options(Connect* req);
int index_dir(Connect* req, std::wstring& path);
//----------------------------------------------------------------------
int ErrorStrSock(const char* f, int line, const char* s);
int PrintError(const char* f, int line, const char* s);
std::string get_time();
void get_time(std::string& s);
std::string log_time();
const char* strstr_case(const char* s1, const char* s2);
int strlcmp_case(const char* s1, const char* s2, int len);
int strcmp_case(const char* s1, const char* s2);

int get_int_method(char* s);
const char* get_str_method(int i);

int get_int_http_prot(char* s);
const char* get_str_http_prot(int i);

const char* strstr_lowercase(const char* s, const char* key);
int clean_path(char* path);

const char* content_type(const wchar_t* path);
int parse_startline_request(Connect* req, char* s, int len);
int parse_headers(Connect* req, char* s, int len);
void path_correct(std::wstring& path);

const char *get_str_operation(OPERATION_TYPE n);
const char *get_cgi_status(CGI_STATUS n);
const char *get_fcgi_status(FCGI_STATUS n);
const char *get_scgi_status(SCGI_STATUS n);
const char *get_cgi_type(CGI_TYPE n);
const char *get_cgi_dir(DIRECT n);
//----------------------------------------------------------------------
int utf16_to_utf8(const std::wstring& ws, std::string& s);
int utf16_to_utf8(const std::wstring& ws, String& s);
int utf8_to_utf16(const char* u8, std::wstring& ws);
int utf8_to_utf16(const std::string& u8, std::wstring& ws);
int utf8_to_utf16(const String& u8, std::wstring& ws);
int decode(const char* s_in, size_t len_in, char* s_out, int len);
std::string encode(const std::string& s_in);
//----------------------------------------------------------------------
int send_message(Connect* req, const char* msg);
int create_response_headers(Connect* req);
int send_response_headers(Connect* req);

int write_timeout(SOCKET sock, const char* buf, size_t len, int timeout);
int send_file_1(SOCKET sock, int fd_in, char* buf, int* size, long long offset, long long* cont_len);
int send_file_2(SOCKET sock, int fd_in, char* buf, int size);
//----------------------------------------------------------------------
void open_logfiles(HANDLE, HANDLE);
void print_err(Connect* req, const char* format, ...);
void print_log(Connect* req);
HANDLE GetHandleLogErr();
//----------------------------------------------------------------------
void end_response(Connect* req);
//----------------------------------------------------------------------
void event_handler(RequestManager* ReqMan);
void push_pollin_list(Connect* req);
void push_send_file(Connect* req);
void push_send_html(Connect* req);
void close_event_handler();
//----------------------------------------------------------------------
void cgi_handler(RequestManager *ReqMan);
void push_cgi(Connect *req);
void close_cgi_handler(void);

#endif
