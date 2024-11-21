/**
 * @author MARCUS AANDAHL <maraa@itu.dk>
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

/* The source code for the proxy is split across three files (including this one). */
#include "proxy.h" // proxy
#include "error.h" // error reporting for ^
#include "http.h"  // http-related things for ^
#include "io.h"    // io-related things for ^

typedef struct {
    int client_fd;
} thread_args;

// Cache-related data structures and functions

typedef struct cache_entry {
    char* key;                   // Unique key (URL)
    char* data;                  // Cached response data
    size_t data_size;            // Size of cached data
    time_t last_accessed;        // Timestamp for LRU tracking
    struct cache_entry* next;    // For linked list management
    struct cache_entry* prev;    // For linked list management
} cache_entry_t;

typedef struct {
    cache_entry_t* head;         // Head of the LRU list
    cache_entry_t* tail;         // Tail of the LRU list
    size_t total_size;           // Current total size of cached objects
    size_t num_entries;          // Current number of cache entries
    pthread_rwlock_t cache_lock; // Readers-writer lock for synchronization
} http_cache_t;

// Global cache instance
static http_cache_t global_cache = {0};

// Cache initialization function
static int cache_init() {
    if (pthread_rwlock_init(&global_cache.cache_lock, NULL) != 0) {
        perror("Failed to initialize cache lock");
        return -1;
    }

    global_cache.head = NULL;
    global_cache.tail = NULL;
    global_cache.total_size = 0;
    global_cache.num_entries = 0;

    return 0;
}

// Internal function to move an entry to the front of LRU list
static void move_to_front(cache_entry_t* entry) {
    if (entry == global_cache.head) {
        return;
    }

    // Unlink entry from its current position
    if (entry->prev) {
        entry->prev->next = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    }

    // Update tail if necessary
    if (entry == global_cache.tail) {
        global_cache.tail = entry->prev;
    }

    // Move to front
    entry->prev = NULL;
    entry->next = global_cache.head;
    if (global_cache.head) {
        global_cache.head->prev = entry;
    }
    global_cache.head = entry;

    // Update tail if list was empty
    if (!global_cache.tail) {
        global_cache.tail = entry;
    }
}

// Internal function to remove the least recently used entry
static void remove_lru_entry() {
    if (!global_cache.tail) {
        return;
    }

    cache_entry_t* lru_entry = global_cache.tail;

    // Update tail
    global_cache.tail = lru_entry->prev;
    if (global_cache.tail) {
        global_cache.tail->next = NULL;
    } else {
        // List is now empty
        global_cache.head = NULL;
    }

    // Update cache metadata
    global_cache.total_size -= lru_entry->data_size;
    global_cache.num_entries--;

    // Free resources
    free(lru_entry->key);
    free(lru_entry->data);
    free(lru_entry);
}

// Cache lookup function
static int cache_lookup(const char* key, char* buffer, size_t* buffer_size) {
    pthread_rwlock_rdlock(&global_cache.cache_lock);

    cache_entry_t* current = global_cache.head;
    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            // Found the entry, copy data if buffer is large enough
            if (*buffer_size >= current->data_size) {
                memcpy(buffer, current->data, current->data_size);
                *buffer_size = current->data_size;

                // Update last accessed time and move to front of LRU list
                current->last_accessed = time(NULL);
                move_to_front(current);

                pthread_rwlock_unlock(&global_cache.cache_lock);
                return 0;
            } else {
                // Buffer too small
                pthread_rwlock_unlock(&global_cache.cache_lock);
                return -1;
            }
        }
        current = current->next;
    }

    // Not found in cache
    pthread_rwlock_unlock(&global_cache.cache_lock);
    return -1;
}

// Cache insert function
static int cache_insert(const char* key, const char* data, size_t data_size) {
    // Validate object size
    if (data_size > MAX_OBJECT_SIZE) {
        return -1;
    }

    // Acquire write lock
    pthread_rwlock_wrlock(&global_cache.cache_lock);

    // Check total cache size before insertion
    while (global_cache.total_size + data_size > MAX_CACHE_SIZE ||
           global_cache.num_entries >= 10) {  // Limit to 10 entries
        remove_lru_entry();
    }

    // Create new cache entry
    cache_entry_t* new_entry = malloc(sizeof(cache_entry_t));
    if (!new_entry) {
        pthread_rwlock_unlock(&global_cache.cache_lock);
        return -1;
    }

    new_entry->key = strdup(key);
    new_entry->data = malloc(data_size);
    if (!new_entry->key || !new_entry->data) {
        free(new_entry->key);
        free(new_entry->data);
        free(new_entry);
        pthread_rwlock_unlock(&global_cache.cache_lock);
        return -1;
    }

    memcpy(new_entry->data, data, data_size);
    new_entry->data_size = data_size;
    new_entry->last_accessed = time(NULL);

    // Add to front of LRU list
    new_entry->next = global_cache.head;
    new_entry->prev = NULL;

    if (global_cache.head) {
        global_cache.head->prev = new_entry;
    }
    global_cache.head = new_entry;

    // Update if tail is not set
    if (!global_cache.tail) {
        global_cache.tail = new_entry;
    }

    // Update cache metadata
    global_cache.total_size += data_size;
    global_cache.num_entries++;

    // Release write lock
    pthread_rwlock_unlock(&global_cache.cache_lock);

    return 0;
}

// Cache cleanup function
static void cache_cleanup() {
    pthread_rwlock_wrlock(&global_cache.cache_lock);

    while (global_cache.head) {
        cache_entry_t* temp = global_cache.head;
        global_cache.head = global_cache.head->next;

        free(temp->key);
        free(temp->data);
        free(temp);
    }

    global_cache.tail = NULL;
    global_cache.total_size = 0;
    global_cache.num_entries = 0;

    pthread_rwlock_unlock(&global_cache.cache_lock);
    pthread_rwlock_destroy(&global_cache.cache_lock);
}

// int main ( int argc, char **argv )
// {
//     int listen_fd; // fd for connection requests from clients.
//
//     /* Check command line args for presence of a port number. */
//     if ( error_args_fatal ( argc, argv ) ) { exit(1); }
//
//     /* Create a `socket`, `bind` it to listen address, configure it to `listen` (for connection requests). */
//     listen_fd = create_listen_fd ( atoi(argv[1]) );
//
//     /* Handle connection requests. */
//     while ( 1 ) {
//        handle_connection_request ( listen_fd );
//     }
//
//     return 0; // Indicates "no error" (although this is never reached).
// }

int main ( int argc, char **argv )
{
    int listen_fd; // fd for connection requests from clients.

    /* Check command line args for presence of a port number. */
    if ( error_args_fatal ( argc, argv ) ) { exit(1); }

    // Initialize cache
    if (cache_init() != 0) {
        fprintf(stderr, "Failed to initialize cache\n");
        exit(1);
    }

    // Register cache cleanup on exit
    atexit(cache_cleanup);

    /* Create a `socket`, `bind` it to listen address, configure it to `listen` (for connection requests). */
    listen_fd = create_listen_fd ( atoi(argv[1]) );

    /* Handle connection requests. */
    while ( 1 ) {
        handle_connection_request ( listen_fd );
    }

    return 0; // Indicates "no error" (although this is never reached).
}

void* handle_request_thread(void* arg) {
    // Detach thread to automatically free resources
    pthread_detach(pthread_self());

    // Extract client file descriptor
    thread_args* args = (thread_args*)arg;
    int client_fd = args->client_fd;
    free(args);  // Free argument structure

    // Call original handle_request function
    handle_request(client_fd);

    // Close client file descriptor
    close(client_fd);

    return NULL;
}

void handle_connection_request(int listen_fd)
{
    int client_fd;   // fd for clients that connect.
    pthread_t thread_id;
    thread_args* args;

    printf("\e[1mawaiting connection request...\e[0m\n");

    // Accept connection
    client_fd = accept(listen_fd, (struct sockaddr*)NULL, NULL);
    if (error_accept_fatal(client_fd)) { exit(1); }
    if (error_accept(client_fd)) { return; }

    // Prepare arguments for thread
    args = malloc(sizeof(thread_args));
    args->client_fd = client_fd;

    // Create detached thread to handle request
    if (pthread_create(&thread_id, NULL, handle_request_thread, args) != 0) {
        perror("Failed to create thread");
        close(client_fd);
        free(args);
        return;
    }

    printf("\e[1mfinished processing request.\e[0m\n");
}

void handle_request ( int client_fd )
{
    int server_fd;                // server file descriptor

    /* String variables */
    char buf[MAX_LINE];
    char method[MAX_LINE];
    char uri[MAX_LINE];
    char version[MAX_LINE];
    char hostname[MAX_LINE];
    char path[MAX_LINE];
    char port[MAX_LINE];
    char request_hdr[MAX_LINE];

    // Buffer for cached response
    char cache_buffer[MAX_OBJECT_SIZE];
    size_t cache_buffer_size;

    int return_cd;
    ssize_t num_bytes;

    /* read HTTP Request-line */
    num_bytes = read_line ( client_fd, buf );
    if ( error_read ( num_bytes ) ) { return; }

    printf("%.*s", (int)num_bytes, buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    /* Ignore non-GET requests (your proxy is only tested on GET requests). */
    if ( error_non_get ( method ) ) { return; }

    /* Parse URI from GET request */
    parse_uri(uri, hostname, path, port);

    /* First, check if response is cached */
    char cache_key[MAX_LINE * 2];  // Combined hostname and path
    snprintf(cache_key, sizeof(cache_key), "%s%s", hostname, path);

    cache_buffer_size = MAX_OBJECT_SIZE;
    if (cache_lookup(cache_key, cache_buffer, &cache_buffer_size) == 0) {
        // Cache hit: send cached response directly to client
        write_all(client_fd, cache_buffer, cache_buffer_size);
        return;
    }

    /* Set the request header */
    return_cd = set_request_header ( request_hdr, hostname, path, port, client_fd );
    if ( error_header ( return_cd ) ) { return; }

    printf("headers: %.*s\n", (int)sizeof(request_hdr), request_hdr);

    /* Create the server fd. */
    server_fd = create_server_fd ( hostname, port );
    if ( error_socket_server ( server_fd ) ) { return; }

    /* Write the request (header) to the server. */
    return_cd = write_all ( server_fd, request_hdr, strlen(request_hdr) );
    if ( error_write_server ( server_fd, return_cd ) ) { return; }

    /* Prepare for caching */
    char response_buffer[MAX_OBJECT_SIZE];
    size_t total_response_size = 0;

   /* Transfer the response from the server, to the client. (until server responds with EOF). */
   do {
      num_bytes = read ( server_fd, buf, MAX_LINE );
      if ( error_read_server  ( server_fd, num_bytes ) ) { return; }

      // Write to client
      num_bytes = write_all ( client_fd, buf, num_bytes );
      if ( error_write_client ( client_fd, num_bytes ) ) { return; }

      // Accumulate response for potential caching
      if (total_response_size + num_bytes <= MAX_OBJECT_SIZE) {
          memcpy(response_buffer + total_response_size, buf, num_bytes);
          total_response_size += num_bytes;
      }
   } while ( num_bytes > 0 );

   // Cache the response if it's within size limits
   if (total_response_size > 0 && total_response_size <= MAX_OBJECT_SIZE) {
       cache_insert(cache_key, response_buffer, total_response_size);
   }

   /* success; close the file descriptor. */
   return_cd = close ( server_fd );
   if ( error_close_server ( return_cd ) ) { /* ignore */ }
}

// void handle_request ( int client_fd )
// {
//     int server_fd;                // server file descriptor
//
//     /* String variables */
//     char buf[MAX_LINE];
//     char method[MAX_LINE];
//     char uri[MAX_LINE];
//     char version[MAX_LINE];
//     char hostname[MAX_LINE];
//     char path[MAX_LINE];
//     char port[MAX_LINE];
//     char request_hdr[MAX_LINE];
//
//     int return_cd;
//     ssize_t num_bytes;
//
//     /* read HTTP Request-line */
//     // printf("\e[1mPRINTF 1\e[0m\n");
//    // read    N bytes from client.
//     num_bytes = read_line ( client_fd, buf );
//     if ( error_read ( num_bytes ) ) { return; }
//
//     /* print what we just read (it's not null-terminated) */
//     // printf("\e[1mPRINTF 2\e[0m\n");
//    // GET http://google.com/ HTTP/1.1
//     printf("%.*s", (int)num_bytes, buf); // typeast is safe; num_bytes <= MAX_LINE
//     sscanf(buf, "%s %s %s", method, uri, version);
//
//     /* Ignore non-GET requests (your proxy is only tested on GET requests). */
//     // printf("\e[1mPRINTF 3\e[0m\n");
//    // success: it is a GET request.
//     if ( error_non_get ( method ) ) { return; }
//
//     /* Parse URI from GET request */
//    // printf("\e[1mPRINTF 4\e[0m\n");
//     // _
//     parse_uri(uri, hostname, path, port);
//
//     /* Set the request header */
//     // printf("\e[1mPRINTF 5\e[0m\n");
//     /*
//         read    18 bytes from client.
//         read    24 bytes from client.
//         read    13 bytes from client.
//         read    30 bytes from client.
//         read     2 bytes from client.
//      */
//     // success: set request header.
//     return_cd = set_request_header ( request_hdr, hostname, path, port, client_fd );
//     if ( error_header ( return_cd ) ) { return; }
//
//     printf("headers: %.*s\n", (int)sizeof(request_hdr), request_hdr);
//
//     /* Create the server fd. */
//     // printf("\e[1mPRINTF 6\e[0m\n");
//     /*
//         success: generate server addresses.
//         HERE
//         HERE
//         success: create server socket & connect.
//     */
//     server_fd = create_server_fd ( hostname, port );
//     if ( error_socket_server ( server_fd ) ) { return; }
//
//     /* Write the request (header) to the server. */
//     // printf("\e[1mPRINTF 7\e[0m\n");
//     // wrote  179 bytes to server.
//     return_cd = write_all ( server_fd, request_hdr, strlen(request_hdr) );
//     if ( error_write_server ( server_fd, return_cd ) ) { return; }
//
//     /* Transfer the response from the server, to the client. (until server responds with EOF). */
//
//    // printf("\e[1mPRINTF 8\e[0m\n");
//     /*
//         read   773 bytes from server.
//         wrote  773 bytes to client.
//         reached end of server fd (EOF).
//         wrote    0 bytes to client.
//      */
//     /* TODO: Start - CACHE
//      * MAX_CACHE_SIZE - for max cache size - only count bytes used to store the actual web objects - any extraneous bytes, including metadata, should be ignored
//      * MAX_OBJECT_SIZE - max size of objects stored
//      * This proxy should cache items smaller than MAX_CACHE_SIZE (100KiB) and in total MAX_CACHE_SIZE (1MiB)
//      * For each active communication, a buffer should be allocated, and accumulated with data as it is received
//      * If the buffer size exceeds MAX_OBJECT_SIZE => discard buffer
//      * Otherwise, cache the response
//      * The cache should use a "least-recently-used" eviction policy
//      * This policy does so the least read OR written object in the cache is evicted when the cache after before writing greater than MAX_CACHE_SIZE-(MAX_CACHE_SIZE*2)
//      * The cache should be thread-safe
//      * The cache should avoid race conditions
//      * Multiple threads must be able to simultaneously read from the cache
//      * Only one thread should be able to write at a time
//      * Protecting accesses to the cache with one large exclusive lock is not an acceptable solution
//     * More valid options include "partitioning the cache", "using Pthreads readers-writers locks", "using semaphores to implement own readers-writers solution"
//      * TODO: End - CACHE
//      */
//    do {
//       num_bytes = read ( server_fd, buf, MAX_LINE );
//       if ( error_read_server  ( server_fd, num_bytes ) ) { return; }
//       num_bytes = write_all ( client_fd, buf, num_bytes );
//       if ( error_write_client ( client_fd, num_bytes ) ) { return; }
//    } while ( num_bytes > 0 );
//
//     /* success; close the file descrpitor. */
//
//    // printf("\e[1mPRINTF 9\e[0m\n");
//     // CLOSE ETC.
//     return_cd = close ( server_fd );
//     if ( error_close_server ( return_cd ) ) { /* ignore */ }
// }

int create_listen_fd ( int port )
{
    /* File descriptors */
    int listen_fd; // fd for connection requests from clients.
    
    /* Return code */
    int return_cd;
    
    /* Socket address (on which proxy shall listen for connection requests) (populated soon). 
       https://man7.org/linux/man-pages/man3/sockaddr.3type.html */
    struct sockaddr_in listen_addr;

    printf("\e[1mcreating listen_fd\e[0m\n");
    
    /* Set socket address (on which proxy shall listen for connection requests). */
    set_listen_socket_address ( &listen_addr, port );
    
    /* "Kernel, make me a socket." (for listening to client connection requests). 
       https://man7.org/linux/man-pages/man2/socket.2.html (a system call) */
    listen_fd = socket( listen_addr.sin_family, SOCK_STREAM, 0 );
    if ( error_socket_fatal ( listen_fd ) ) { exit(1); }

    /* "Kernel, if you think the address I'm binding to is already in use, then
       this socket may reuse the address." (optional)
       NOTE: quality-of-life; it takes kernel ~1 min to free up an address; w/o 
       this, after proxy stopped, you have to wait a bit before you can start again).
       https://man7.org/linux/man-pages/man2/setsockopt.2.html (a system call) */
    return_cd = setsockopt( listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int) );
    if ( error_socket_option ( return_cd ) ) { /* ignore */ }
    
    /* "Kernel, bind it to this socket address" (i.e. where proxy shall listen). 
       https://man7.org/linux/man-pages/man2/bind.2.html (a system call) */
    return_cd = bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
    if ( error_bind_fatal ( return_cd ) ) { exit(1); }

    /* "Kernel, oh btw, that socket? Make it passive." (it's for connection requests)
       https://man7.org/linux/man-pages/man2/listen.2.html (a system call) */
    return_cd = listen(listen_fd, LISTENQ);
    if ( error_listen_fatal ( return_cd ) ) { exit(1); }

    printf("\e[1mlisten_fd ready\e[0m\n");
    
    return listen_fd;
}

/* set the proxy socket address (where it listens for connection requests). */
void set_listen_socket_address ( struct sockaddr_in *listen_addr, int port )
{
    memset( listen_addr, '0', sizeof(struct sockaddr_in) ); // zero out the address
    listen_addr->sin_family = AF_INET;
    listen_addr->sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr->sin_port = htons(port);
    /* NOTE: we /should/ use `getnameinfo` & `getaddrinfo` (in real world, so should you).
       with `getaddrinfo`, we get a list of potential socket addresses, and for each
       socket address in the list, we should attempt to create + bind a socket to it
       (stopping on the first successful `socket` (i.e. create)  and 'bind'). why:
        * more robust (can bind to 32-bit and 256-bit addresses, whichever server has) 
        * more secure (an attacker on `cos` cannot hijack this socket by 
                       binding to a more specific address than INADDR_ANY).
       instead, here we hard-code port, pick 32-bit IP addresses, and all available interfaces.
       why: because I know cos supports this, and it is simpler; `getaddrinfo` is 
       intimidating for the uninitiated. (why: check out the server socket code.) */
    printf("\033[32msuccess:\033[0m set socket address of proxy.\n");
}

int create_server_fd ( char* hostname, char* port )
{
    int server_fd;
    int return_cd;
    
    struct addrinfo *cand_ai; // pointer to heap-allocated candidate server addresses (free this!)

    /* Get list of candidate server socket addresses. */
    return_cd = get_server_socket_address_candidates ( &cand_ai, hostname, port );
    if ( error_address_server ( return_cd ) ) { return -1; }

    struct addrinfo *curr_ai; // pointer to current candidate server address in the above list.

    /* produces a socket (server_fd) bound to the first candidate address (in cand_ai)
       for which creating (resp. binding) a socket for (resp. to) it was successful. */
    for ( curr_ai = cand_ai; curr_ai != NULL; curr_ai = curr_ai->ai_next ) {
	/* "Kernel, make me a socket." (for curr_ai)
	   https://man7.org/linux/man-pages/man2/socket.2.html (a system call) */
    printf("HERE\n");
	server_fd = socket ( curr_ai->ai_family, curr_ai->ai_socktype, curr_ai->ai_protocol );
    printf("HERE\n");
	if ( server_fd == -1 )
	    continue; // try the next ai.

	/* "Kernel, please (attempt to) connect to said socket."
	   https://man7.org/linux/man-pages/man2/connect.2.html (a system call) */
        return_cd = connect ( server_fd, curr_ai->ai_addr, curr_ai->ai_addrlen );
	//return_cd = connect ( server_fd, (struct sockaddr *)&curr_ai, sizeof(curr_ai) );
	if ( return_cd < 0 ) { printf("failure connecting to socket. trying next one.\n"); }
	if ( return_cd == 0 )
	    break;    // success

	/* couldn't bind the socket to curr_ai. try the next ai. */
	close( server_fd );
    }
    /* free up the heap-allocated linked list. */
    freeaddrinfo(curr_ai);
    
    /* report errors if any. */
    if ( return_cd < 0 ) { return -1; }

    /* success; return the server fd. */
    return server_fd;
}

int get_server_socket_address_candidates ( struct addrinfo **cand_ai, char* hostname, char* port )
{
    struct addrinfo hints_ai; // hints for proposing candidate server addresses (i.e. for generating cand_ai)
    /* set hints. network socket, numeric port, avoid IPv6 socket for hosts that don't support those. */ 
    memset ( &hints_ai, 0, sizeof(struct addrinfo) );
    hints_ai.ai_socktype = SOCK_STREAM;
    hints_ai.ai_flags    = AI_NUMERICSERV | AI_ADDRCONFIG;
    return getaddrinfo ( hostname, port, &hints_ai, cand_ai );
}
