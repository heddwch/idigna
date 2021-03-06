#define _GNU_SOURCE // For memrchr

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <poll.h>
#include <netinet/in.h>
#include <unistd.h>

long int server_port = 80;

const char default_itemtype = '0'; // Default to text file
const char *default_mimetype = "application/octet-stream"; // Default to binary mime-type

struct { char itemtype; const char *mimetype; } mimetypes[] = {
	{'0', "text/plain; charset=utf-8"}, // Text file
	{'4', "application/binhex"}, // BinHex archive
	{'5', "application/octet-stream"}, //Binary archive
	{'6', "text/x-uuencode"}, // UUEncoded file
	{'9', "application/octet-stream"}, // Binary file
	{'g', "image/gif"}, // GIF image
	{'h', "text/html; charset=utf-8"}, // HTML document
};

struct { const char *ext; const char *mimetype; } extension_mimetypes[] = {
	{".jpg", "image/jpeg"},
	{".jpeg", "image/jpeg"},
	{".png", "image/png"},
	{".wav", "audio/wav"},
	{".mp3", "audio/mpeg"}
};

const char *program_name;
const char *remote;
long int remote_port = 70;
char remote_port_string[6];

struct pollfd *sockets = NULL;
size_t number_sockets = 0;
size_t number_interfaces = 0;

enum connection_state { START, PATH, REQUEST_END, CONNECT, REQUEST_WRITE, HEADER_WRITE, READ, WRITE };
enum copymode { TEXT, BINARY, GOPHERMAP };
struct connection {
	enum connection_state state;

	int sock;
	int sock_other;

	char *path;
	size_t path_size;

	char itemtype;
	enum copymode copymode;

	char *buffer;
	size_t buffer_size;

	size_t written;
	size_t read;
	bool beginning_of_line;
};

struct connection **connections = NULL;
size_t number_connections = 0;
bool use_syslog = false;

void usage(FILE *stream) {
	fprintf(stream, "%s [--daemon|-d] [--port|-p server_port] remote [remote_port]\n", program_name);
}

void help(FILE *stream) {
	usage(stream);
}

void log_error(const char * restrict format, ...) {
	va_list args;
	va_start(args, format);
	if(use_syslog) {
		vsyslog(LOG_ERR, format, args);
	} else {
		vfprintf(stderr, format, args);
	}
	va_end(args);

	return;
}

long int parse_port(const char *string) {
	char *endptr;
	long int port = strtol(string, &endptr, 10);

	if(endptr == string || *endptr != '\0') { // String did not fully scan as number
		return -1;
	} else if(port < 0 || port > 1<<16) { // Port value out of range
		return -1;
	} else { // All ok
		return port;
	}
}

bool stringify_port(long int port, char *buffer, size_t buffer_length) {
	int size = snprintf(buffer, buffer_length, "%li", port);

	// If snprintf returns either an error signal or size larger than buffer, signal error
	if(size < 0 || (size_t)size > buffer_length) {
		return false;
	} else {
		return true;
	}
}

void add_socket(int sock, short events) {
	// Grow the table of sockets
	size_t index = number_sockets++;
	sockets = realloc(sockets, number_sockets * sizeof(struct pollfd));

	if(sockets == NULL) {
		perror("realloc");
		exit(1);
	}

	// Add socket to the table
	struct pollfd new_socket = {.fd = sock, .events = events};
	sockets[index] = new_socket;
}

void remove_socket(size_t index) {
	// Clean the socket up
	close(sockets[index].fd);

	if(index != number_sockets - 1) {
		// The socket entry was not at the end of the table -> we need to rearrange to allow shrinking of allocation
		memmove(&sockets[index], &sockets[number_sockets - 1], sizeof(*sockets));
	}

	// Now the last entry in the table is either the socket to remove or a duplicate -> safe to remove either way
	// Shrink the allocation and update number_sockets
	sockets = realloc(sockets, --number_sockets * sizeof(struct pollfd));

	if(sockets == NULL && number_sockets != 0) {
		perror("realloc");
		exit(1);
	}
}

size_t get_socket_index(int sock) {
	for(size_t i = 0; i < number_sockets; i++) {
		if(sockets[i].fd == sock) {
			return i;
		}
	}

	// None found, return index of last element + 1
	return number_sockets;
}

void add_connection(int sock) {
	// Grow the table of connections
	size_t index = number_connections++;
	connections = realloc(connections, number_connections * sizeof(struct connection));

	if(connections == NULL) {
		perror("realloc");
		exit(1);
	}

	// Add socket to table of sockets
	add_socket(sock, POLLIN);

	// Initialise and add connection to the table
	struct connection *connection = calloc(1, sizeof(struct connection));

	if(connection == NULL && sizeof(struct connection) != 0) {
		perror("calloc");
		exit(1);
	}

	connection->state = START;
	connection->sock = sock;
	connection->sock_other = -1;

	connections[index] = connection;
}

void remove_connection(size_t index) {
	// Clean the connection up
	size_t socket_index = get_socket_index(connections[index]->sock);
	if(socket_index == number_sockets) {
		log_error("%s: socket to remove not in table of sockets\n", program_name);
		exit(1);
	}
	remove_socket(socket_index);

	if(connections[index]->sock_other != -1) {
		close(connections[index]->sock_other);
	}

	if(connections[index]->path != NULL) {
		free(connections[index]->path);
	}

	if(connections[index]->buffer != NULL) {
		free(connections[index]->buffer);
	}

	if(index != number_connections - 1) {
		// The connection was not at the end of the table -> we need to rearrange to allow shrinking of allocation
		memmove(&connections[index], &connections[number_connections - 1], sizeof(*connections));
	}

	// Now the last entry in the table is either the connection to remove or a duplicate -> safe to remove either way
	// Shrink the allocation and update number_connections
	connections = realloc(connections, --number_connections * sizeof(struct connection));

	if(connections == NULL && number_connections != 0) {
		perror("realloc");
		exit(1);
	}
}

size_t get_connection_index(int sock) {
	for(size_t i = 0; i < number_connections; i++) {
		if(connections[i]->sock == sock) {
			return i;
		}
	}

	// None found, return index of last element + 1
	return number_connections;
}

void add_listen(struct addrinfo *res) {
	const int yes = 1;

	// Create socket
	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(sock == -1) {
		perror("socket");
		exit(1);
	}

	// Disable the IPv4 over IPv6, as that results in IPv4 and IPv6 sockets conflicting and one of them not being able to be set up
	if(res->ai_family == AF_INET6) {
		if(setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
			perror("setsockopt");
			exit(1);
		}
	}

	// Set reuseaddr
	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
		perror("setsockopt");
		exit(1);
	}

	// Bind onto given address
	if(bind(sock, res->ai_addr, res->ai_addrlen) == -1) {
		perror("bind");
		exit(1);
	}

	// Listen for incoming connections
	if(listen(sock, 1) == -1) {
		perror("listen");
		exit(1);
	}

	add_socket(sock, POLLIN);
}

void setup_listen(unsigned long port) {
	char port_string[6];

	// getaddrinfo wants port as a string, so stringify it
	if(!stringify_port(port, port_string, sizeof(port_string))) {
		log_error("%s: Could not convert %li to string\n", program_name, port);
		exit(1);
	}

	struct addrinfo hints;
	struct addrinfo *getaddrinfo_result;

	// AF_UNSPEC: either IPv4 or IPv6
	// SOCK_STREAM: TCP
	// AI_PASSIVE: fill out my IP for me
	// AI_ADDRCONFIG: only return addresses I have a configured interface for
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

	int status = getaddrinfo(NULL, port_string, &hints, &getaddrinfo_result);

	if(status != 0) {
		log_error("%s: getaddrinfo failed: %s\n", program_name, gai_strerror(status));
		exit(1);
	}

	for(struct addrinfo *res = getaddrinfo_result; res != NULL; res = res->ai_next) {
		// Add corresponding interface to table of sockets
		add_listen(res);
	}

	freeaddrinfo(getaddrinfo_result);

	// Store the number of sockets corresponding to interfaces, as they won't be the only sockets in the table
	number_interfaces = number_sockets;
}

int connect_to_remote(void) {
	struct addrinfo hints;
	struct addrinfo *getaddrinfo_result;

	// AF_UNSPEC: either IPv4 or IPv6
	// SOCK_STREAM: TCP
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	int status = getaddrinfo(remote, remote_port_string, &hints, &getaddrinfo_result);

	if(status != 0) {
		log_error("%s: getaddrinfo failed: %s\n", program_name, gai_strerror(status));
		exit(1);
	}

	for(struct addrinfo *res = getaddrinfo_result; res != NULL; res = res->ai_next) {
		// Create socket
		int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if(sock == -1) {
			perror("socket");
			exit(1);
		}

		// Connect to remote
		if(connect(sock, res->ai_addr, res->ai_addrlen) == -1) {
			close(sock);
			continue;
		}

		return sock;
	}

	return -1;
}

void buffer_append(char **buffer, size_t *buffer_length, char *appended, size_t appended_length) {
	*buffer = realloc(*buffer, *buffer_length + appended_length);

	if (*buffer == NULL && *buffer_length + appended_length != 0) {
		perror("realloc");
		exit(1);
	}

	memmove(*buffer + *buffer_length, appended, appended_length);
	*buffer_length = *buffer_length + appended_length;
}

void *memdup(const void *mem, size_t size) {
	void *dup = malloc(size);
	if(dup == NULL && size != 0) {
		perror("malloc");
		exit(1);
	}
	memmove(dup, mem, size);
	return dup;
}

void switch_sockets(struct connection *conn) {
	int tmp = conn->sock_other;
	conn->sock_other = conn->sock;
	conn->sock = tmp;
}

void socket_change(int old, int new, short events) {
	size_t socket_index = get_socket_index(old);
	if(socket_index == number_sockets) {
		log_error("%s: socket requested is not in list of sockets\n", program_name);
		exit(1);
	}
	sockets[socket_index].fd = new;
	sockets[socket_index].events = events;
}

bool recognised_itemtype(char itemtype) {
	return (
		itemtype == '0' || // Text file
		itemtype == '1' || // Gopher directory listing
		itemtype == '4' || // BinHex archive
		itemtype == '5' || // Binary archive
		itemtype == '6' || // UUEncoded file
		itemtype == '9' || // Binary file
		itemtype == 'g' || // GIF image
		itemtype == 'h' || // HTML document
		itemtype == 'I' || // Image (generic)
		itemtype == 's' // Sound
	);
}

void get_itemtype_selector(char *itemtype, char **selector, size_t *selector_length, const char *path, size_t path_length) {
	const char *start = path;
	size_t left = path_length;

	// Ignore a / on the beginning of path
	if(left > 0 && start[0] == '/') {
		start++;
		left--;
	}

	// Special case: null-length selector has itemtype 1
	// Otherwise, if we recognise the first character of path as itemtype -> save it as itemtype and move selector's beginning one forward
	// If not, set itemtype to default itemtype and don't move beginning of selector
	if(left == 0) {
		*itemtype = '1';
	} else if(left >= 1 && recognised_itemtype(start[0])) {
		*itemtype = start[0];
		start++;
		left--;
	} else {
		*itemtype = default_itemtype;
	}

	// Create a new buffer and copy selector to it
	*selector = memdup(start, left);
	*selector_length = left;
}

const char *get_mimetype(char itemtype, const char *selector, size_t selector_length) {
	// Special handling for itemtype 1
	// Eventually, it will be translated into HTML, but for time being it'll be plain text
	if(itemtype == '1') {
		return "text/plain; charset=utf-8";
	}

	// Special handling for itemtypes I and s
	if(itemtype == 'I' || itemtype == 's') {
		char *extension = memrchr(selector, '.', selector_length);
		ssize_t extension_legth = selector_length - (extension - selector);
		if(extension == NULL) {
			// There is no extension, everything is a lie
			return default_mimetype;
		}

		// Create a C string from the buffer + length
		extension = strndup(extension, extension_legth);
		if(extension == NULL) {
			perror("strndup");
			exit(1);
		}

		for(size_t i = 0; i < sizeof(extension_mimetypes) / sizeof(*extension_mimetypes); i++) {
			if(strcmp(extension, extension_mimetypes[i].ext) == 0) {
				free(extension);
				return extension_mimetypes[i].mimetype;
			}
		}

		// Unrecognised extension
		free(extension);
		return default_mimetype;
	}

	for(size_t i = 0; i < sizeof(mimetypes) / sizeof(*mimetypes); i++) {
		if(itemtype == mimetypes[i].itemtype) {
			return mimetypes[i].mimetype;
		}
	}

	// Nothing matched
	return default_mimetype;
}


enum copymode get_copymode(char itemtype) {
	if(itemtype == '1') { // Gopher directory listing
		return GOPHERMAP;
	} else if(itemtype == '0' || itemtype == '4' || itemtype == '6' || itemtype == 'h') { // Text file, UUEncoded file, HTML document
		return TEXT;
	} else {
		return BINARY;
	}
}

void handle_connection(size_t index) {
	struct connection *conn = connections[index];

	if(conn->state == START || conn->state == PATH) {
		// Read data (that's what we're here for) and append to buffer
		char buffer[1024];
		ssize_t amount = recv(conn->sock, &buffer, sizeof(buffer), 0);

		if(amount <= 0) {
			// EOF or error
			remove_connection(index);
			return;
		}

		buffer_append(&conn->buffer, &conn->buffer_size, buffer, amount);
	} else if(conn->state == REQUEST_END) {
		// Read data and keep the last 4 bytes (only interested in \r\n\r\n)
		char buffer[1024];
		size_t buffer_fill = conn->buffer_size;
		memmove(buffer, conn->buffer, buffer_fill);
		ssize_t amount = recv(conn->sock, &buffer + buffer_fill, sizeof(buffer) - buffer_fill, 0);

		if(amount <= 0) {
			// EOF or error
			remove_connection(index);
			return;
		}

		buffer_fill += amount;

		if(buffer_fill < 4) {
			memmove(conn->buffer, buffer, buffer_fill);
			conn->buffer_size = buffer_fill;
		} else {
			memmove(conn->buffer, &buffer[buffer_fill - 4], 4);
			conn->buffer_size = 4;
		}
	}

	if(conn->state == START) {
		// Check buffer's contents to see if we can move to next state
		if(conn->buffer_size >= 4 && memcmp(conn->buffer, "GET ", 4) == 0) {
			// Remove the first 4 bytes (not needed by us) from the buffer
			conn->buffer_size = conn->buffer_size - 4;
			memmove(conn->buffer, conn->buffer + 4, conn->buffer_size);

			conn->state = PATH;
		}
	}

	if(conn->state == PATH) {
		char *path_end = memchr(conn->buffer, ' ', conn->buffer_size);
		if(path_end != NULL) {
			// Copy the path from buffer into separate path buffer
			conn->path_size = path_end - conn->buffer;
			conn->path = memdup(conn->buffer, conn->path_size);

			// Copy max. 4 bytes off the end, in case it has \r\n\r\n
			size_t left_over = conn->buffer_size - conn->path_size;
			char tmpbuf[4];
			size_t tmpbuf_size;
			if(left_over < 4) {
				memmove(&tmpbuf, path_end, left_over);
				tmpbuf_size = left_over;
			} else {
				memmove(&tmpbuf, &conn->buffer[conn->buffer_size - 4], 4);
				tmpbuf_size = 4;
			}

			// Free the buffer and replace it with a tiny one, store the copied bytes
			free(conn->buffer);
			conn->buffer = malloc(4);
			if(conn->buffer == NULL) {
				perror("malloc");
				exit(1);
			}
			memmove(conn->buffer, &tmpbuf, tmpbuf_size);
			conn->buffer_size = tmpbuf_size;

			conn->state = REQUEST_END;
		}
	}

	if(conn->state == REQUEST_END) {
		if(conn->buffer_size >= 4 && memcmp(conn->buffer, "\r\n\r\n", 4) == 0) {
			// Completely remove the buffer
			free(conn->buffer);
			conn->buffer = NULL;
			conn->buffer_size = 0;

			conn->state = CONNECT;
		}
	}

	if(conn->state == CONNECT) {
			// Connect to remote and change it to our main socket
			size_t remote_socket = connect_to_remote();
			conn->sock_other = conn->sock;
			conn->sock = remote_socket;

			// Change socket in the table of sockets
			socket_change(conn->sock_other, remote_socket, POLLOUT);

			// Separate itemtype and selector
			char *path;
			size_t path_size;
			get_itemtype_selector(&conn->itemtype, &path, &path_size, conn->path, conn->path_size);
			free(conn->path);
			conn->path = path;
			conn->path_size = path_size;

			// Put conn->path to conn->buffer and append \r\n to it to create a valid request
			conn->buffer = memdup(conn->path, conn->path_size);
			conn->buffer_size = conn->path_size;
			buffer_append(&conn->buffer, &conn->buffer_size, "\r\n", 2);

			conn->state = REQUEST_WRITE;
			// Do not continue onwards to REQUEST_WRITE's code, because we changed the socket mid-function and REQUEST_WRITE's code assumes function got called with POLLOUT revent
			return;
	}

	if(conn->state == REQUEST_WRITE) {
		char *start = conn->buffer + conn->written;
		size_t left = conn->buffer_size - conn->written;
		ssize_t amount = send(conn->sock, start, left, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		conn->written += amount;

		if(conn->written >= conn->buffer_size) {
			// Completely remove the old buffer containig the request
			free(conn->buffer);
			conn->buffer = NULL;
			conn->buffer_size = 0;

			// Create new buffer with HTTP response
			const char *mimetype = get_mimetype(conn->itemtype, conn->path, conn->path_size);
			char *response;
			int response_size = asprintf(&response, "HTTP/1.1 200 OK\r\nContent-type: %s\r\n\r\n", mimetype);
			if(response_size < 0) {
				perror("asprintf");
				exit(1);
			}
			conn->buffer = response;
			conn->buffer_size = response_size;

			// Set amount written to 0
			conn->written = 0;

			// Switch socket and change to write mode
			switch_sockets(conn);
			socket_change(conn->sock_other, conn->sock, POLLOUT);

			conn->state = HEADER_WRITE;
			// Return because socket was changed
			return;
		}
	}

	if(conn->state == HEADER_WRITE) {
		char *start = conn->buffer + conn->written;
		size_t left = conn->buffer_size - conn->written;
		ssize_t amount = send(conn->sock, start, left, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		conn->written += amount;

		if(conn->written >= conn->buffer_size) {
			// Completely remove the old buffer containig the header
			free(conn->buffer);
			conn->buffer = NULL;
			conn->buffer_size = 0;

			// Allocate a fixed buffer for data copying
			conn->buffer = malloc(1024);
			if(conn->buffer == NULL) {
				perror("malloc");
				exit(1);
			}
			conn->buffer_size = 1024;

			// Set copying mode
			conn->copymode = get_copymode(conn->itemtype);

			// Set conn->beginning_of_line in case copymode uses that information
			conn->beginning_of_line = true;

			// Switch socket and change to read mode
			switch_sockets(conn);
			socket_change(conn->sock_other, conn->sock, POLLIN);

			conn->state = READ;
			// Return because socket was changed
			return;
		}
	}

	if(conn->state == READ) {
		ssize_t amount = recv(conn->sock, conn->buffer, conn->buffer_size, 0);

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		if(amount == 0) {
			// EOF reached
			remove_connection(index);
			return;
		}

		// Store the amount of data that's been read into the buffer and reset the amount written
		conn->read = amount;
		conn->written = 0;

		// Switch socket and change to write mode
		switch_sockets(conn);
		socket_change(conn->sock_other, conn->sock, POLLOUT);

		conn->state = WRITE;
		// Return because socket was changed
		return;
	}
	
	if(conn->state == WRITE) {
		ssize_t amount;
		ssize_t skipped = 0;

		if(conn->copymode == GOPHERMAP) {
			log_error("Gophermap copymode not yet supported, substituting text copymode\n");
			conn->copymode = TEXT;
		}

		if(conn->copymode == BINARY) {
			char *start = conn->buffer + conn->written;
			size_t left = conn->read - conn->written;
			amount = send(conn->sock, start, left, 0);
		} else if(conn->copymode == TEXT) {
			char *start = conn->buffer + conn->written;
			size_t max_left = conn->read - conn->written;

			if(conn->beginning_of_line && max_left >= 2 && memcmp(start, "..", 2) == 0) {
				// Remove the double period in the beginning of line
				start++;
				max_left--;
				skipped += 1;
			} else if(conn->beginning_of_line && max_left >= 3 && memcmp(start, ".\r\n", 3) == 0) {
				// Close connection
				remove_connection(index);
				return;
			}

			char *end = memchr(start, '\n', max_left);

			size_t left;
			if(end == NULL) {
				left = max_left;
				conn->beginning_of_line = false;
			} else {
				// Include the \n in the sent text as well
				left = end - start + 1;
				conn->beginning_of_line = true;
			}

			amount = send(conn->sock, start, left, 0);
		} else {
			log_error("%s: Illegal value of conn->copymode: %i", program_name, conn->copymode);
			exit(1);
		}

		if(amount == -1) {
			remove_connection(index);
			return;
		}

		conn->written += amount + skipped;

		if(conn->written >= conn->read) {
			// Switch socket and change to read mode
			switch_sockets(conn);
			socket_change(conn->sock_other, conn->sock, POLLIN);

			conn->state = READ;
			// Return because socket was changed
			return;
		}
	}
}

void drop_privileges(void) {
	uid_t uid = getuid();
	gid_t gid = getgid();

	if(setresgid(gid, gid, gid) != 0) {
		perror("setresgid");
		exit(1);
	}
	if(setresuid(uid, uid, uid) != 0) {
		perror("setresuid");
		exit(1);
	}
}


int main(int argc, char **argv) {
	// Store proram name for later use
	if(argc < 1) {
		log_error("Missing program name\n");
		exit(1);
	} else {
		char *argv0 = strdup(argv[0]);
		if(argv0 == NULL) {
			perror("strdup");
			exit(1);
		}
		program_name = strdup(basename(argv0));
		if(program_name == NULL) {
			perror("strdup");
			exit(1);
		}
		free(argv0);
	}

	// Do option handling
	struct option long_options[] = {
		{"help", no_argument, 0, 0},
		{"port", required_argument, 0, 'p'},
		{"daemon", no_argument, 0, 'd'},
		{0, 0, 0, 0}
	};

	for(;;) {
		int long_option_index;
		int opt = getopt_long(argc, argv, "dp:", long_options, &long_option_index);
		// Used for daemonization
		pid_t child;
		int fd;

		if(opt == -1) {
			break;
		}

		switch(opt) {
			case 0: // Long option with no short equivalent
				if(strcmp(long_options[long_option_index].name, "help") == 0) {
					help(stdout);
					exit(0);
				}
				break;;

			case 'd': // Daemonize
				use_syslog = true;
				if((child = fork()) < 0) {
					perror("fork");
					exit(1);
				}
				if(child > 0) {
					exit(0);
				}
				if(setsid() < 0) {
					perror("setsid");
					exit(1);
				}
				signal(SIGCHLD, SIG_IGN);
				signal(SIGHUP, SIG_IGN);
				if((child = fork()) < 0) {
					perror("fork");
					exit(1);
				}
				if(child > 0) {
					exit(0);
				}
				umask(0);
				chdir("/");
				for(fd = sysconf(_SC_OPEN_MAX); fd > 0; --fd) {
					close(fd);
				}
				stdin = fopen("/dev/null", "r");
				stderr = stdout = fopen("/dev/null", "w+");
				break;;

			case 'p':
				server_port = parse_port(optarg);
				if(server_port < 0) {
					usage(stderr);
					exit(1);
				}
				break;;

			default:
				usage(stderr);
				exit(1);
		}
	}

	if(optind == argc - 2) {
		// 2 arguments left -> remote and remote_port
		remote = argv[optind];
		remote_port = parse_port(argv[optind + 1]);
		if(remote_port < 0) {
			usage(stderr);
			exit(1);
		}
	} else if(optind == argc - 1) {
		// 1 argument left -> only remote
		remote = argv[optind];
	} else {
		usage(stderr);
		exit(1);
	}

	// getaddrinfo wants port as a string, so stringify it
	if(!stringify_port(remote_port, remote_port_string, sizeof(remote_port_string))) {
		log_error("%s: Could not convert %li to string\n", program_name, remote_port);
		exit(1);
	}

	// Populate the table of sockets with all possible sockets to listen on
	setup_listen(server_port);

	// Drop privileges or die trying
	drop_privileges();

	// Poll
	while(1) {
		int amount_ready = poll(sockets, number_sockets, -1);
		if(amount_ready < 0) {
			perror("poll");
			exit(1);
		}

		for(size_t i = 0; i < number_sockets && amount_ready > 0; i++) {
			// While the order of sockets in the table gets rearranged if one is removed, the rearrangement only affects sockets created after the removed one
			// Thus, as long as an interface is not removed from the table, all sockets < number_interfaces are interfaces and other data sockets
			if(i < number_interfaces) {
				// Interface socket
				if(sockets[i].revents & POLLIN) {
					struct sockaddr_storage client_addr;
					socklen_t addr_size = sizeof(client_addr);

					int sock = accept(sockets[i].fd, (struct sockaddr *)&client_addr, &addr_size);

					add_connection(sock);

					amount_ready--;
				}
			} else {
				if(sockets[i].revents & (POLLHUP | POLLIN | POLLOUT)) {
					// Data socket
					size_t connection_index = get_connection_index(sockets[i].fd);

					if(connection_index == number_connections) {
						log_error("%s: socket does not correspond to any connection\n", program_name);
						exit(1);
					}

					if(sockets[i].revents & POLLHUP) {
						remove_connection(connection_index);
					} else {
						handle_connection(connection_index);
					}

					amount_ready--;
				}
			}
		}
	}
}
