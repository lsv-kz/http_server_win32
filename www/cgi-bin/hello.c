#include <stdio.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

//======================================================================
int main()
{
	fflush(stdin);
	fflush(stdout);
	
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	
	printf("Content-Type: text/plain\r\n"
			"\r\n"
			"Hello from script");
	return 0;
}
