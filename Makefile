CFLAGS = -Wall -O2 -std=c++11 -m32
CC = g++.exe

OBJSDIR = objs

OBJS = $(OBJSDIR)/server.o \
	$(OBJSDIR)/config.o \
	$(OBJSDIR)/create_socket.o \
	$(OBJSDIR)/log.o \
	$(OBJSDIR)/create_headers.o \
	$(OBJSDIR)/response.o \
	$(OBJSDIR)/range.o \
	$(OBJSDIR)/encoding.o \
	$(OBJSDIR)/functions.o \
	$(OBJSDIR)/rd_wr.o \
	$(OBJSDIR)/threads_manager.o \
	$(OBJSDIR)/event_handler.o \
	$(OBJSDIR)/cgi.o \
	$(OBJSDIR)/fcgi.o \
	$(OBJSDIR)/index.o \

server.exe: $(OBJS)
	$(CC) $(CFLAGS) -o $@  $(OBJS)  -lwsock32 -lws2_32 -static -static-libgcc -static-libstdc++

$(OBJSDIR)/server.o: server.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/config.o: config.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/create_socket.o: create_socket.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/log.o: log.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/create_headers.o: create_headers.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/response.o: response.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/range.o: range.cpp main.h classes.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/event_handler.o: event_handler.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/encoding.o: encoding.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/functions.o: functions.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/send_files.o: send_files.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/rd_wr.o: rd_wr.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/threads_manager.o: threads_manager.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/cgi.o: cgi.cpp main.h classes.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/fcgi.o: fcgi.cpp main.h classes.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJSDIR)/index.o: index.cpp main.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	del -f server.exe
	del -f $(OBJSDIR)\*.o
