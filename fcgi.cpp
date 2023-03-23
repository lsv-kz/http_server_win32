#include "classes.h"

using namespace std;

#define FCGI_RESPONDER  1
//#define FCGI_AUTHORIZER 2
//#define FCGI_FILTER     3

//#define FCGI_KEEP_CONN  1

#define FCGI_VERSION_1           1
#define FCGI_BEGIN_REQUEST       1
#define FCGI_ABORT_REQUEST       2
#define FCGI_END_REQUEST         3
#define FCGI_PARAMS              4
#define FCGI_STDIN               5
#define FCGI_STDOUT              6
#define FCGI_STDERR              7
#define FCGI_DATA                8
#define FCGI_GET_VALUES          9
#define FCGI_GET_VALUES_RESULT  10
#define FCGI_UNKNOWN_TYPE       11
#define FCGI_MAXTYPE (FCGI_UNKNOWN_TYPE)

typedef struct {
    unsigned char type;
    int len;
    int paddingLen;
} fcgi_header;

const int requestId = 1;
//======================================================================
