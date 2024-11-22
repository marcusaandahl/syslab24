/**
 * @author MARCUS AANDAHL <maraa@itu.dk>
 */

#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

/* The source code for the proxy is split across three files (including this one). */
#include "proxy.h" // proxy
#include "error.h" // error reporting for ^
#include "http.h"
#include "io.h"    // io-related things for ^

// Cache entry struct
typedef struct cache_entry {
    char* url; // URL as key
    char* data; // Cached response
    size_t size; // Response size
    struct cache_entry* next; // Next entry pointer (like linked list)
} cache_entry_t;

// Cache struct
static struct {
    cache_entry_t* head; // Latest cache item
    size_t total_size; // Cache size
    pthread_rwlock_t lock; // read-write lock
} cache = {NULL, 0, PTHREAD_RWLOCK_INITIALIZER};

// Thread args struct
typedef struct {
    int client_fd;
} thread_args;

static void cache_init() {
    pthread_rwlock_init(&cache.lock, NULL);
}

static void cache_cleanup() {
    pthread_rwlock_wrlock(&cache.lock);

    cache_entry_t* current = cache.head;
    while (current != NULL) {
        cache_entry_t* next = current->next;
        free(current->url);
        free(current->data);
        free(current);
        current = next;
    }

    cache.head = NULL;
    cache.total_size = 0;

    pthread_rwlock_unlock(&cache.lock);
    pthread_rwlock_destroy(&cache.lock);
}

// Look up a URL in the cache
static int cache_lookup(const char* url, char* buffer, size_t* size) {
    cache_entry_t* entry = cache.head;
    while (entry) {
        if (strcmp(entry->url, url) == 0) {
            if (*size >= entry->size) {
                memcpy(buffer, entry->data, entry->size);
                *size = entry->size;

                // If hit is head -> continue
                cache_entry_t* entry_hit = cache.head;
                if (strcmp(entry_hit->url, url) == 0) { return 0; }

                // Move cache hit to head of cache
                cache_entry_t* entry_before_hit = cache.head;
                entry_hit = entry_hit->next;

                while (entry_hit) {
                    if (strcmp(entry_hit->url, url) == 0) {
                        // Is hit
                        entry_before_hit->next = entry_hit->next; // Skip entry hit in cache
                        // Move entry hit to front
                        entry_hit->next = cache.head->next;
                        cache.head = entry_hit;
                        return 0;
                    }
                    entry_before_hit = entry_hit;
                    entry_hit = entry_hit->next;
                }


                cache.head = entry;

                return 0;
            }
            return -1;
        }
        entry = entry->next;
    }
    return -1;
}

static void cache_insert(const char* url, const char* data, size_t size) {
    if (size > MAX_OBJECT_SIZE) return;

    pthread_rwlock_wrlock(&cache.lock);

    // Make space by removing older entries
    while (
        cache.total_size + size > MAX_CACHE_SIZE
        && cache.head != NULL
    ) {
        cache_entry_t* last = cache.head;
        cache_entry_t* prev = NULL;

        // Find oldest entry
        while (last->next != NULL) {
            prev = last;
            last = last->next;
        }

        // Remove it
        if (prev) prev->next = NULL;
        else cache.head = NULL;

        cache.total_size -= last->size;
        free(last->url);
        free(last->data);
        free(last);
    }

    // Create new entry
    cache_entry_t* new_entry = malloc(sizeof(cache_entry_t));
    new_entry->url = strdup(url);
    new_entry->data = malloc(size);
    memcpy(new_entry->data, data, size);
    new_entry->size = size;

    // Add to front of list
    new_entry->next = cache.head;
    cache.head = new_entry;

    cache.total_size += size;

    pthread_rwlock_unlock(&cache.lock);
}

int main ( int argc, char **argv )
{
    /* Check command line args for presence of a port number. */
    if ( error_args_fatal ( argc, argv ) ) { exit(1); }

    // Initialize cache
    cache_init();
    atexit(cache_cleanup);

    /* Create a `socket`, `bind` it to listen address, configure it to `listen` (for connection requests). */
    const int listen_fd = create_listen_fd(atoi(argv[1]));
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to create listening socket\n");
        return 1;
    }

    /* Handle connection requests. */
    while ( 1 ) {
        handle_connection_request ( listen_fd );
    }
}

void* handle_request_thread(void* arg) {
    pthread_detach(pthread_self());

    thread_args* args = (thread_args*)arg;
    int client_fd = args->client_fd;
    free(args);

    handle_request(client_fd);
    close(client_fd);

    return NULL;
}

void handle_connection_request(int listen_fd)
{
    printf("\e[1mawaiting connection request...\e[0m\n");

    /* "Kernel, give me the fd of a connected socket for the next connection request."
       NOTE: this blocks the proxy until a connection arrives.
       https://man7.org/linux/man-pages/man2/accept.2.html (a system call) */
    const int client_fd = accept(listen_fd, NULL, NULL);
    if (error_accept_fatal(client_fd)) { exit(1); }
    if (error_accept(client_fd)) { return; }

    // Create thread args and spawn new thread
    thread_args* args = malloc(sizeof(thread_args));
    args->client_fd = client_fd;
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, handle_request_thread, args) != 0) {
        perror("Failed to create thread");
        close(client_fd);
        free(args);
        return;
    }

    printf("\e[1mspawned new thread for request.\e[0m\n");
}

void handle_request(int client_fd) {
    char buf[MAX_LINE];
    char method[32], uri[MAX_LINE], version[32];
    char hostname[MAX_LINE], path[MAX_LINE], port[16];
    char request_hdr[MAX_LINE];

    /* read HTTP Request-line */
    ssize_t num_bytes = read_line(client_fd, buf);
    if ( error_read ( num_bytes ) ) { return; }

    // Parse request line
    sscanf(buf, "%s %s %s", method, uri, version);

    /* Ignore non-GET requests (your proxy is only tested on GET requests). */
    if ( error_non_get ( method ) ) { return; }

    // Check cache first
    size_t response_size = MAX_OBJECT_SIZE;
    char response_buffer[MAX_OBJECT_SIZE];
    if (cache_lookup(uri, response_buffer, &response_size) == 0) {
        // Cache hit - send directly to client
        write(client_fd, response_buffer, response_size);
        return;
    }

    // Cache miss - need to fetch from server
    // Parse URI to get hostname, path, and port
    parse_uri(uri, hostname, path, port);

    /* Set the request header */
    const int return_cd = set_request_header ( request_hdr, hostname, path, port, client_fd );
    if ( error_header ( return_cd ) ) { return; }

    /* Create the server fd. */
    const int server_fd = create_server_fd(hostname, port);
    if ( error_socket_server ( server_fd ) ) { return; }

    // Send request to server
    if (write_all(server_fd, request_hdr, strlen(request_hdr)) < 0) {
        close(server_fd);
        return;
    }

    // Read server response and store for caching
    size_t total_size = 0;
    while ((num_bytes = read(server_fd, buf, MAX_LINE)) > 0) {
        // Forward data to client
        if (write_all(client_fd, buf, num_bytes) < 0) {
            close(server_fd);
            return;
        }

        // Store in response buffer for caching if there's space
        if (total_size + num_bytes <= MAX_OBJECT_SIZE) {
            memcpy(response_buffer + total_size, buf, num_bytes);
            total_size += num_bytes;
        }
    }

    // Store response in cache if it's not too large
    if (total_size > 0 && total_size <= MAX_OBJECT_SIZE) {
        cache_insert(uri, response_buffer, total_size);
    }

    close(server_fd);
}

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
