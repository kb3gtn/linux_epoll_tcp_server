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

#include <vector>
#include <string>
#include <cstring>
#include <iostream>
#include <array>
#include <thread>
#include <mutex>
#include <list>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <signal.h>
#include <chrono>
#include <memory>
#include <algorithm>

// linux headers for sockets and epoll
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>

using namespace std;

atomic<bool> AppRunning;

void sig_handler(int signo) {
  AppRunning.store(false);
}

// max number of epoll events to handle in 1 go..
// This is the max number of events the kernel will dispatch
// to us per epoll_wait() call..
constexpr int tcp_epoll_max_events = 32;

class TCP_Server {
  public:
    //* constructor to define bind interface and port
    TCP_Server(string localHost, uint16_t localPort);
    //* constructor to define just local port, assumes 0.0.0.0 as local host.
    TCP_Server(uint16_t localPort);
    //* destructor
    ~TCP_Server();
    // tell if TCP server is running
    bool isAlive() { return isRunning; }
  private:
    int socketfd; // server socket..
    int create_and_bind(const string &localHost, const uint16_t &localPort);
    bool make_socket_nonblocking( int socketfd);
    int accept_connection(int socketfd, struct epoll_event& event, int epollfd);

    /////////////////////////////////////////////////////////////////////
    // overload this function to handle events for your appplication..
    // implements a simple echo server. Received messages are sent to all connected clients.
    void event_worker();

    // internal methods to start/stop event_worker thread.
    void start_event_worker();
    void stop_event_worker();
    // member datastructures
    atomic<bool> worker_state; // 0 -- offline, 1 -- online, set by worker thread.
    atomic<bool> isRunning; // when true, tells worker to keep running. False signals worker to stop.
    thread epoll_worker;
    struct epoll_event event;
    array<struct epoll_event, ::tcp_epoll_max_events> events; // list of events to handle
    vector<int> client_fd_list;
};

///////////////////////////////////////////////////////
// Constructor specifing bind host and port
TCP_Server::TCP_Server(string localHost, uint16_t localPort) {
  if ( create_and_bind( localHost, localPort) == 0 ) {
    // bound successfully, start event handling thread.
    start_event_worker();
  }
}

//////////////////////////////////////////////////////
// Constructor specifing port only, host 0.0.0.0 is assumed.
TCP_Server::TCP_Server(uint16_t localPort) {
  if ( create_and_bind( string("0.0.0.0"), localPort) == 0 ) {
    // bound successfully, start event handling thread.
    start_event_worker();
  }
}

//////////////////////////////////////////////////////
// Destructor to shutdown service threads..
TCP_Server::~TCP_Server() {
  stop_event_worker();
}

int TCP_Server::create_and_bind(const string &localHost, const uint16_t &localPort) {
  // create TCP/IP V4 socket..
  if (( socketfd=socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    std::cerr << "[E] failed to create IPv4 socket..\n";
    return -1;
  }

  // make it so we use our port if we are killed.
  int sockoptsargs = 1;
  if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &sockoptsargs, sizeof(int)) < 0)
     std::cerr << "[W] setsockopt(SO_REUSEADDR) failed.. May get bind error..\n";

  // build address struct for bind to use..
  struct sockaddr_in address;
  memset(&address, 0, sizeof(sockaddr_in));
  address.sin_family = AF_INET;

  std::cout << "Debug: localHost = \"" << localHost << "\"\n";

  if ( localHost.compare("INADDR_ANY") != 0 ) {
    // lookup provided hostname to address, use DNS/HOSTFILE if needed..
    struct hostent *hp = gethostbyname(localHost.c_str());
    if ( hp == NULL ) {
      std::cerr << "[E] gethostbyname() failed.  Unable to resolve hostname: " << localHost << "\n";
      return -1;
    }
    std::cout << "[N] setting up listener on: " << inet_ntoa( *(struct in_addr*)(hp->h_addr_list[0])) << ":" << localPort << "\n";
    memcpy(&address.sin_addr, hp->h_addr_list[0], hp->h_length);
  } else {
    // bind to any interface 0.0.0.0
    std::cout << "[N] setting up listener on: INADDR_ANY:" << localPort << "\n";
    address.sin_addr.s_addr = INADDR_ANY; 
  }
  // use port provided..
  address.sin_port = htons(localPort);

  // bind to port..
  if ( bind( socketfd, (struct sockaddr*)&address, sizeof(sockaddr_in) ) != 0 ) {
    std::cerr << "[E] failed to bind() to interface..\n";
    close(socketfd);
    return -1;
  }
  return 0;
}

bool TCP_Server::make_socket_nonblocking( int socketfd) {
  int flags = fcntl(socketfd, F_GETFL, 0);
  if (flags == -1) {
    std::cerr << "[E] fcntl failed (F_GETFL)\n";
    return false;
  }

  flags |= O_NONBLOCK;
  int s = fcntl(socketfd, F_SETFL, flags);
  if (s == -1) {
    std::cerr << "[E] fcntl failed (F_SETFL)\n";
    return false;
  }
  return true; 
}

// called by event_worker to accept new connections
int TCP_Server::accept_connection(int socketfd, struct epoll_event& event, int epollfd) {
  struct sockaddr in_addr;
  socklen_t in_len = sizeof(in_addr);
  int infd = accept(socketfd, &in_addr, &in_len);
  if (infd == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) // Done processing incoming connections
    {
      return -1;
    }
    else
    {
      std::cerr << "[E] accept failed\n";
      return -1;
    }
  }
  std::string hbuf(NI_MAXHOST, '\0');
  std::string sbuf(NI_MAXSERV, '\0');
  if (getnameinfo(&in_addr, in_len,
                  const_cast<char*>(hbuf.data()), hbuf.size(),
                  const_cast<char*>(sbuf.data()), sbuf.size(),
                  NI_NUMERICHOST | NI_NUMERICSERV) == 0)
  {
    std::cout << "[I] Accepted connection as client " << infd << "(host=" << hbuf << ", port=" << sbuf << ")" << "\n";
  }

  if (!make_socket_nonblocking(infd))
  {
    std::cerr << "[E] make_socket_nonblocking failed\n";
    return -1;
  }

  // add accepted connection FD to epoll list..
  event.data.fd = infd;
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, infd, &event) == -1)
  {
    std::cerr << "[E] epoll_ctl failed\n";
    return -1;
  } 
  return infd;
}

////////////////////////////////////
// This is the magic thread worker
// update this part for your project..
void TCP_Server::event_worker() {
  // startup
  // create listener
  if ( listen(socketfd, SOMAXCONN) == -1 ) {
    std::cerr << "[E] Failed to create socket listener.. Exit..\n";
    return;
  }

  event.data.fd = socketfd; // class members..
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;

  int epollfd = epoll_create1(0);
  if (epollfd == -1) {
    std::cerr << "[E] epoll_create1 failed..  Worker Exit..\n";
    return;
  }
  
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, socketfd, &event) == -1 ) {
    std::cerr << "[E] epoll_ctl add poll request failed..\n";
    return;
  }

  worker_state.store(true);

  while ( isRunning.load(memory_order_acquire) == true ) {
    // wait untill kernel has between 1 - 32 events for us to process.
    // timesout after 500 milliseconds. (for polling if thread should die or not.)
    auto n = epoll_wait( epollfd, events.data(), ::tcp_epoll_max_events, 500 );

    // if timed out, n=0 and the for loop will not run..
    for (int i = 0; i < n; ++i)
    {
      cout << "event[" << i << "] = " << events[i].events << std::endl;

      if (events[i].events & EPOLLERR ||
        events[i].events & EPOLLHUP ||
        !(events[i].events & EPOLLIN)) // error
      {
        std::cerr << "[E] epoll event error detected\n";
        close(events[i].data.fd);
      }
      else if (socketfd == events[i].data.fd) // new connection
      {
        std::cerr << "[N] accepting a new connection..\n";
        int newclientfd = accept_connection(socketfd, event, epollfd);
        if ( newclientfd > 0 )
          client_fd_list.push_back(newclientfd);
      }
      else // data to read  (simple echo server..)
      {
        int fd = events[i].data.fd;
        // do stuff to read and handle input data from client.
        char bufin[1024];
        int size = read(fd, &bufin, 1024);
        if ( size > 0 ) {
          std::cerr << "[N] received message of " << size << " bytes from client " << fd << endl;
          if ( ( size == 6 ) && ( memcmp("quit", &bufin, 4) == 0 )) {
            std::cerr << "[I] client " << fd << " sent quit message. Closing socket..\n";
            vector<int>::iterator it;
            it = find(client_fd_list.begin(), client_fd_list.end(), fd);
            if ( it != client_fd_list.end() ) {
              std::cerr << "[I] Removed client " << fd << " from client list..\n";
              client_fd_list.erase(it); // remove client form list
            }
            close(fd);
          } else {
            std::cerr << "  forwarding into clients: ";
            for( auto sendfd : client_fd_list) {
              if ( sendfd != fd) {
                std::cerr << sendfd << " ";
                write(sendfd, &bufin, size );
              }
            } 
            std::cerr << "\n";
          }
        } else {
          std::cerr << "Client " << fd << " read_error, closing socket..\n";
          vector<int>::iterator it;
          it = find(client_fd_list.begin(), client_fd_list.end(), fd);
          if ( it != client_fd_list.end() ) {
            std::cerr << "[I] Removed client " << fd << " from client list..\n";
            client_fd_list.erase(it); // remove client form list
          }
          close(fd);
        }
      }
    }
    cout << flush; // force screen up after this loop.
  }

  // shutdown
  std::cerr << "[N] Worker thread shutting down.." << std::endl;
  worker_state.store(false); // notify watchers that we are no longer running.
  close(socketfd); // close listener socket and epoll requests.
  close(epollfd);
}

void TCP_Server::start_event_worker() {
  std::cerr << "[N] TCP_Server starting event thread..\n";
  // init state
  worker_state.store(false); // 0 -- offline, 1 -- online, set by worker thread.
  isRunning.store(true); // when true, tells worker to keep running. False signals worker to stop.
  // start worker thread.
  epoll_worker = thread(&TCP_Server::event_worker, this);
}

void TCP_Server::stop_event_worker() {
  std::cerr << "[N] TCP_Server shutting down event thread..\n";
  isRunning.store(false, memory_order_acquire); // False signals worker to stop.
  std::cerr << "[N] Waiting for server worker thread to exit..\n";
  epoll_worker.join(); // wait for thread to exit..
  std::cerr << "[N] server worker thread shutdown complete..\n";
}

/////////////////////////////////////
// Main
int main() {
  // register signal handler.
  signal(SIGINT, sig_handler);
  AppRunning.store(true);


  TCP_Server myTCPServer(9090);
  // wait 1 second before check to see if TCP_Server started correctly..
  std::this_thread::sleep_for (std::chrono::seconds(1)); 
  if (! myTCPServer.isAlive()) {
    std::cerr << "[E] TCP Server appears to have failed to start correctly. Exit..\n";
    return -1;
  } else {
    std::cerr << "[N] TCP Server worker has started succesfully..\n";
  }

  std::cout << "Press Ctrl-C (SIGINT) to exit.." << std::endl;

  while (AppRunning.load() == true) {
    // wait for ctrl-c to be pressed.

  }

  std::cerr << "\n[N] Main Loop Exit.. Starting Shutdown..\n";

  return 0;
}
