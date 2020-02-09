# linux_epoll_tcp_server
Example TCP Server using linux EPOLL

//////////////////////////////////////////////  
// TCP EPOLL SERVER   
//   
// Simple example program to show how EPOLL   
// networking works in linux.   
//   
// This project receive messages from a client and forwards it   
// to all connected clients.   
//   
// Peter Fetterer (kb3gtn@gmail.com)  
//   
/////////////////////////////////////////////  
// Build instructions:  (Linux only)  
// g++ tcp_epoll_server.cpp -o tcp_epoll_server -lpthread  
//  
/////////////////////////////////////////////   
// Quick Operation guide   
// once compiled, run ./tcp_epoll_server in a termnal   
// Open up 2 (or more) terminals and run "telnet localhost 9090"   
// At the telnet prompt type stuff.  It should show in other connect telnet sessions.   
//   
// Pressing CTRL-C in the termnal running tcp_epoll_server will cause the    
// tcp server to shutdown, disconnecting all clients.   
//   
// In telnet pressing 'ctrl-v' followed by ']' will bring up the telnet prompt   
// in which typing quit will cause the telnet session to close.   
//   
// Sending the message 'quit' followed by return will cause the tcp server to   
// close the session as well.   
//   
/////////////////////////////////////////////////////   
   

