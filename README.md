# cpp-vndb-console-userpage
Test/Proof-of-concept program to demonstrate usage of sockets within TCP API context.

Incorporates threading to ensure non-blockage of socket read/write and main program loop. 
Basic menu system with simple keywords to navigate.
"Direct" mode to direct query server.

Currently supported usages:
- lastmod10,     display last 10 modified visual novels
- lookup-user,   query server for username/UID or labels associated with username/UID
- help,          display help menu containing possible keyword commands
- config,        enter interactive configuration menu to setup UID and save to file
- direct,        enter direct mode which forwards console input to server and prints any server response

Aim to use 0 platform dependant code, currently only calls 1 Windows only function to set up console output to UTF-8.
Program should work on most Linux and Mac systems, provided Boost C++ library support and C++11 compiler support.
