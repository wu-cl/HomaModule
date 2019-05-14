// This file contains a collection of tests for the Linux implementation
// of Homa
//
// Usage:
// homaTest host:port [options] op op ...
//
// host:port gives the location of a server to invoke
// Each op specifies a particular test to perform

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>

#include <thread>

#include "homa.h"
#include "test_utils.h"

/**
 * get_int() - Parse an integer from a string, and exit if the parse fails.
 * @s:      String to parse.
 * @msg:    Error message to print (with a single %s specifier) on errors.
 * Return:  The integer value corresponding to @s.
 */
int get_int(const char *s, const char *msg)
{
	int value;
	value = strtol(s, NULL, 10);
	if (value == 0) {
		printf(msg, s);
		exit(1);
	}
	return value;
}

/**
 * close_fd() - Helper method for "close" test: sleeps a while, then closes
 * an fd
 * @fd:   Open file descriptor to close.
 */
void close_fd(int fd)
{
	sleep(1);
	if (close(fd) >= 0) {
		printf("Closed fd %d\n", fd);
	} else {
		printf("Close failed on fd %d: %s\n", fd, strerror(errno));
	}
}

/**
 * send_fd() - Helper method for "poll" test: sleeps a while, then sends
 * a request to a socket.
 * @fd:      File descriptor for a Homa socket; used to send the message.
 * @addr:    Where to send the message.
 * @request: Request message to send.
 * @length:  Number of bytes in @request.
 */
void send_fd(int fd, struct sockaddr *addr, char *request, int length)
{
	uint64_t id;
	int status;
	
	sleep(1);
	status = homa_send(fd, request, length, addr, sizeof(*addr), &id);
	if (status < 0) {
		printf("Error in homa_send: %s\n",
			strerror(errno));
	} else {
		printf("Homa_send succeeded, id %lu\n", id);
	}
}

/**
 * shutdown_fd() - Helper method for "close" test: sleeps a while, then shuts
 * down an fd
 * @fd:   Open file descriptor to shut down.
 */
void shutdown_fd(int fd)
{
	sleep(1);
	if (shutdown(fd, 0) >= 0) {
		printf("Shutdown fd %d\n", fd);
	} else {
		printf("Shutdown failed on fd %d: %s\n", fd, strerror(errno));
	}
}

/**
 * print_help() - Print out usage information for this program.
 * @name:   Name of the program (argv[0])
 */
void print_help(const char *name)
{
	printf("Usage: %s host:port [options] op op ...\n\n"
		"host:port describes a server to communicate with, and each op\n"
		"selects a particular test to run (see the code for available\n"
		"tests). The following options are supported:\n\n"
		"--count      Number of times to repeat a test (default: 1000)\n"
		"--length     Size of messages, in bytes (default: 100)\n"
		"--seed       Used to compute message contents (default: 12345)\n",
		name);
}

/**
 * test_close() - Close a Homa socket while a thread is waiting on it.
 */
void test_close()
{
	int result, fd;
	int message[100000];
	struct sockaddr source;
	uint64_t id = 0;

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		printf("Couldn't open Homa socket: %s\n",
			strerror(errno));
		exit(1);
	}
	std::thread thread(close_fd, fd);
	result = homa_recv(fd, message, sizeof(message), HOMA_RECV_RESPONSE,
			&id, &source, sizeof(source));
	if (result > 0) {
		printf("Received %d bytes\n", result);
	} else {
		printf("Error in recvmsg: %s\n",
			strerror(errno));
	}
}

/**
 * test_fill_memory() - Send requests to a server, but never read responses;
 * eventually, this will cause memory to fill up.
 * @fd:       Homa socket.
 * @dest:     Where to send the request
 * @request:  Request message.
 * @length:   Number of bytes in @request.
 * @count:    How many requests to send.
 */
void test_fill_memory(int fd, struct sockaddr *dest, char *request, int length,
	int count)
{
	uint64_t id;
	int status;
	int completed = 0;
	size_t total = 0;
#define PRINT_INTERVAL 1000
	char buffer[length];
	struct sockaddr_in src_addr;
	ssize_t received;
	uint64_t start = rdtsc();

	for (int i = 1; i <= count; i++) {
		status = homa_send(fd, request, length, dest,
				sizeof(*dest), &id);
		if (status < 0) {
			printf("Error in homa_send: %s\n",
				strerror(errno));
			sleep(1);
		}
		total += length;
		if ((i % PRINT_INTERVAL) == 0) {
			printf("%lu MB sent (%d RPCs)\n", total/1000000, i);
		}
	}
	total = 0;
	for (int i = 1; i <= count; i++) {
		id = 0;
		received = homa_recv(fd, buffer, length, HOMA_RECV_RESPONSE,
				&id, (struct sockaddr *) &src_addr,
				sizeof(src_addr));
		if (received < 0) {
			printf("Error in homa_recv for id %lu: %s\n",
				id, strerror(errno));
		} else {
			total += received;
			completed++;
		}
		if ((i % PRINT_INTERVAL) == 0) {
			printf("%lu MB received (%d RPCs)\n", total/1000000, i);
		}
	}
	uint64_t end = rdtsc();
	double tput = total;
	tput = tput / to_seconds(end-start);
	double timePer = to_seconds(end-start) / completed;
	printf("%d/%d RPCs succeeded, average goodput %.1f MB/sec (%.1f us/RPC)\n",
		completed, count, tput*1e-06, timePer*1e06);
}

/**
 * test_invoke() - Send a request and wait for response.
 * @fd:       Homa socket.
 * @dest:     Where to send the request
 * @request:  Request message.
 * @length:   Number of bytes in @request.
 */
void test_invoke(int fd, struct sockaddr *dest, char *request, int length)
{
	uint64_t id = 0;
	char response[100000];
	struct sockaddr_in server_addr;
	int status;
	ssize_t resp_length;

	status = homa_send(fd, request, length, dest,
			sizeof(*dest), &id);
	if (status < 0) {
		printf("Error in homa_send: %s\n",
			strerror(errno));
	} else {
		printf("Homa_send succeeded, id %lu\n", id);
	}
	resp_length = homa_recv(fd, response, sizeof(response),
		HOMA_RECV_RESPONSE, &id, (struct sockaddr *) &server_addr,
		sizeof(server_addr));
	if (resp_length < 0) {
		printf("Error in homa_recv: %s\n",
			strerror(errno));
		return;
	}
	int seed = check_buffer(response, resp_length);
	printf("Received message from %s with %lu bytes, "
		"seed %d, id %lu\n",
		print_address(&server_addr), resp_length, seed, id);
}

/**
 * test_poll() - Receive a message using the poll interface.
 * @fd:       Homa socket.
 * @request:  Request message.
 * @length:   Number of bytes in @request.
 */
void test_poll(int fd, char *request, int length)
{
	uint64_t id;
	int result;
	int message[100000];
	struct pollfd poll_info = {
		.fd =     fd,
		.events = POLLIN,
		.revents = 0
	};
	struct sockaddr_in source;
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(500);
	
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		printf("Couldn't bind socket to Homa port %d: %s\n",
				ntohl(addr.sin_port), strerror(errno));
		return;
	}
	
	std::thread thread(send_fd, fd, (struct sockaddr *) &addr,
			request, length);
	thread.detach();
	
	result = poll(&poll_info, 1, -1);
	if (result > 0) {
		printf("Poll succeeded with mask 0x%x\n", poll_info.revents);
	} else {
		printf("Poll failed: %s\n", strerror(errno));
		return;
	}
	
	result = homa_recv(fd, message, sizeof(message), HOMA_RECV_REQUEST,
			&id, (struct sockaddr *) &source, sizeof(source));
	if (result < 0) {
		printf("homa_recv failed: %s\n", strerror(errno));
	} else {
		printf("homa_recv returned %d bytes from port %d\n",
				result, ntohs(source.sin_port));
	}
}

/**
 * test_rtt() - Measure round-trip time for an RPC.
 * @fd:       Homa socket.
 * @dest:     Where to send the request
 * @request:  Request message.
 * @length:   Number of bytes in @request.
 * @count:    Number of RPCs to issue.
 */
void test_rtt(int fd, struct sockaddr *dest, char *request, int length,
		int count)
{
	uint64_t id;
	char response[100000];
	struct sockaddr_in server_addr;
	int status;
	ssize_t resp_length;
	uint64_t start;
	uint64_t times[count];

	for (int i = -10; i < count; i++) {
		start = rdtsc();
		status = homa_send(fd, request, length, dest,
				sizeof(*dest), &id);
		if (status < 0) {
			printf("Error in homa_send: %s\n",
				strerror(errno));
			return;
		}
		resp_length = homa_recv(fd, response, sizeof(response),
			HOMA_RECV_RESPONSE, &id,
			(struct sockaddr *) &server_addr, sizeof(server_addr));
		if (i >= 0)
			times[i] = rdtsc() - start;
		if (resp_length < 0) {
			printf("Error in homa_recv: %s\n",
				strerror(errno));
			return;
		}
	}
	print_dist(times, count);
}

/**
 * test_send() - Send a request; don't wait for response.
 * @fd:       Homa socket.
 * @dest:     Where to send the request
 * @request:  Request message.
 * @length:   Number of bytes in @request.
 */
void test_send(int fd, struct sockaddr *dest, char *request, int length)
{
	uint64_t id;
	int status;

	status = homa_send(fd, request, length, dest,
			sizeof(*dest), &id);
	if (status < 0) {
		printf("Error in homa_send: %s\n",
			strerror(errno));
	} else {
		printf("Homa_send succeeded, id %lu\n", id);
	}
}

/**
 * test_shutdown() - Shutdown a Homa socket while a thread is waiting on it.
 * @fd:   Homa socket
 */
void test_shutdown(int fd)
{
	int result;
	int message[100000];
	struct sockaddr source;
	uint64_t id = 0;

	std::thread thread(shutdown_fd, fd);
	thread.detach();
	result = homa_recv(fd, message, sizeof(message), HOMA_RECV_RESPONSE,
			&id, &source, sizeof(source));
	if (result > 0) {
		printf("Received %d bytes\n", result);
	} else {
		printf("Error in homa_recv: %s\n",
			strerror(errno));
	}
	
	/* Make sure that future reads also fail. */
	result = homa_recv(fd, message, sizeof(message), HOMA_RECV_RESPONSE,
			&id, &source, sizeof(source));
	if (result < 0) {
		printf("Second homa_recv call also failed: %s\n",
			strerror(errno));
	} else {
		printf("Second homa_recv call succeeded: %d bytes\n", result);
	}
}

/**
 * test_udpclose() - Close a UDP socket while a thread is waiting on it.
 */
void test_udpclose()
{
	/* Test what happens if a UDP socket is closed while a
	 * thread is waiting on it. */
	struct sockaddr_in address;
	char buffer[1000];

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		printf("Couldn't open UDP socket: %s\n",
			strerror(errno));
		exit(1);
	}
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = 0;
	int result = bind(fd,
		reinterpret_cast<struct sockaddr*>(&address),
		sizeof(address));
	if (result < 0) {
		printf("Couldn't bind UDP socket: %s\n",
			strerror(errno));
		exit(1);
	}
	std::thread thread(close_fd, fd);
	thread.detach();
	result = read(fd, buffer, sizeof(buffer));
	if (result >= 0) {
		printf("UDP read returned %d bytes\n", result);
	} else {
		printf("UDP read returned error: %s\n",
			strerror(errno));
	}
}

int main(int argc, char** argv)
{
	int fd, status, port, nextArg;
	struct addrinfo *matching_addresses;
	struct sockaddr *dest;
	struct addrinfo hints;
	char *host, *port_name;
	int seed = 12345;
	char buffer[HOMA_MAX_MESSAGE_LENGTH];
	int length = 100;
	int count = 1000;
	
	if ((argc >= 2) && (strcmp(argv[1], "--help") == 0)) {
		print_help(argv[0]);
		exit(0);
	}
	
	if (argc < 3) {
		printf("Usage: %s host:port [options] op op ...\n", argv[0]);
		exit(1);
	}
	host = argv[1];
	port_name = strchr(argv[1], ':');
	if (port_name == NULL) {
		printf("Bad server spec %s: must be 'host:port'\n", argv[1]);
		exit(1);
	}
	*port_name = 0;
	port_name++;
	port = get_int(port_name,
			"Bad port number %s; must be positive integer\n");
	for (nextArg = 2; (nextArg < argc) && (*argv[nextArg] == '-');
			nextArg += 1) {
		if (strcmp(argv[nextArg], "--help") == 0) {
			print_help(argv[0]);
			exit(0);
		} else if (strcmp(argv[nextArg], "--count") == 0) {
			if (nextArg == (argc-1)) {
				printf("No value provided for %s option\n",
					argv[nextArg]);
				exit(1);
			}
			nextArg++;
			count = get_int(argv[nextArg],
					"Bad count %s; must be positive integer\n");
		} else if (strcmp(argv[nextArg], "--length") == 0) {
			if (nextArg == (argc-1)) {
				printf("No value provided for %s option\n",
					argv[nextArg]);
				exit(1);
			}
			nextArg++;
			length = get_int(argv[nextArg],
				"Bad message length %s; must be positive "
				"integer\n");
			if (length > HOMA_MAX_MESSAGE_LENGTH) {
				length = HOMA_MAX_MESSAGE_LENGTH;
				printf("Reducing message length to %d\n", length);
			}
		} else if (strcmp(argv[nextArg], "--seed") == 0) {
			if (nextArg == (argc-1)) {
				printf("No value provided for %s option\n",
					argv[nextArg]);
				exit(1);
			}
			nextArg++;
			seed = get_int(argv[nextArg],
				"Bad seed %s; must be positive integer\n");
		} else {
			printf("Unknown option %s; type '%s --help' for help\n",
				argv[nextArg], argv[0]);
			exit(1);
		}
	}
	
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	status = getaddrinfo(host, "80", &hints, &matching_addresses);
	if (status != 0) {
		printf("Couldn't look up address for %s: %s\n",
				host, gai_strerror(status));
		exit(1);
	}
	dest = matching_addresses->ai_addr;
	((struct sockaddr_in *) dest)->sin_port = htons(port);
	seed_buffer(buffer, sizeof(buffer), seed);
	
	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		printf("Couldn't open Homa socket: %s\n", strerror(errno));
		exit(1);
	}
	
	for ( ; nextArg < argc; nextArg++) {
		if (strcmp(argv[nextArg], "close") == 0) {
			test_close();
		} else if (strcmp(argv[nextArg], "fill_memory") == 0) {
			test_fill_memory(fd, dest, buffer, length, count);
		} else if (strcmp(argv[nextArg], "invoke") == 0) {
			test_invoke(fd, dest, buffer, length);
		} else if (strcmp(argv[nextArg], "poll") == 0) {
			test_poll(fd, buffer, length);
		} else if (strcmp(argv[nextArg], "send") == 0) {
			test_send(fd, dest, buffer, length);
		} else if (strcmp(argv[nextArg], "rtt") == 0) {
			test_rtt(fd, dest, buffer, length, count);
		} else if (strcmp(argv[nextArg], "shutdown") == 0) {
			test_shutdown(fd);
		} else if (strcmp(argv[nextArg], "udpclose") == 0) {
			test_udpclose();
		} else {
			printf("Unknown operation '%s'\n", argv[nextArg]);
			exit(1);
		}
	}
	close(fd);
	exit(0);
}
