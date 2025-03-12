// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define PATH_SIZE 50

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;
// int total = 0, completed = 0;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	strcpy(conn->send_buffer, "HTTP/1.1 200 OK\r\n"
			"Content-Length: ");
	char file_size[13] = {0};

	sprintf(file_size, "%lu\r\n", conn->file_size);
	strcat(conn->send_buffer, file_size);
	// content type????????
	strcat(conn->send_buffer, "Connection: close\r\n"
			"\r\n");
	// now, only data itself needs to be added to send_buffer
	// set send_pos to length of header so that data is written from that point
	conn->send_pos = strlen(conn->send_buffer);
	// make send_len equal to header length
	conn->send_len = conn->send_pos;
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	strcpy(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n\r\n");
	// make send_len equal to header length
	conn->send_len = strlen(conn->send_buffer);
	conn->state = STATE_SENDING_404;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	char *found = NULL, *file_name, path_to_file[PATH_SIZE];
	int rc;

	found = strstr(conn->request_path, AWS_REL_STATIC_FOLDER);
	if (found) {
		file_name = found + strlen(AWS_REL_STATIC_FOLDER);
		// create the path to file using the macros and the file name
		strcpy(path_to_file, AWS_ABS_STATIC_FOLDER);
		strcat(path_to_file, file_name);
		// try to access file requested with read permissions
		rc = access(path_to_file, R_OK);
		if (rc < 0) {
			// file does not exist or does not have read permissions
			return RESOURCE_TYPE_NONE;
		}
		return RESOURCE_TYPE_STATIC;
	}
	found = strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER);
	if (found) {
		file_name = found + strlen(AWS_REL_DYNAMIC_FOLDER);
		// create the path to file using the macros and the file name
		strcpy(path_to_file, AWS_ABS_DYNAMIC_FOLDER);
		strcat(path_to_file, file_name);
		// try to access file requested with read permissions
		rc = access(path_to_file, R_OK);
		if (rc < 0) {
			// file does not exist or does not have read permissions
			return RESOURCE_TYPE_NONE;
		}
		return RESOURCE_TYPE_DYNAMIC;
	}
	// does not access the static or dynamic paths
	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = calloc(1, sizeof(struct connection));

	conn->sockfd = sockfd;
	conn->eventfd = eventfd(0, 0);
	DIE(conn->eventfd < 0, "eventfd");
	conn->state = STATE_INITIAL;
	conn->ctx = ctx;
	// make piocb point to iocb
	conn->piocb[0] = &conn->iocb;
	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	int rc;
	// set count to space left to read from file or BUFSIZ
	// (whichever is smaller)
	size_t count = conn->file_size < BUFSIZ - conn->send_pos ?
						 conn->file_size : BUFSIZ - conn->send_pos;
	// if it is first time here
	if (conn->state == STATE_REQUEST_RECEIVED) {
		// if there is enough space in buffer for whole file
		if (conn->file_size < BUFSIZ - conn->send_pos)
			count = conn->file_size;
		else
			count = BUFSIZ - conn->send_pos;
	} else {
		// if there is less than BUFSIZ to read
		if (conn->file_size - conn->file_pos < BUFSIZ)
			count = conn->file_size - conn->file_pos;
		else
			count = BUFSIZ;
	}

	// start writing in send buffer at position after HTTP header
	memset(&conn->iocb, 0, sizeof(conn->iocb));
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer + conn->send_pos,
				  count, conn->file_pos);
	// it represents the position TO which is read from file
	conn->file_pos += count;
	// update send_len with number to be read from file
	conn->send_len += count;
	io_set_eventfd(&conn->iocb, conn->eventfd);
	// add eventfd to epoll if its first time here
	if (conn->state == STATE_REQUEST_RECEIVED) {
		rc = w_epoll_add_ptr_out(epollfd, conn->eventfd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_out");
	}
	rc = io_submit(conn->ctx, 1, conn->piocb);
	DIE(rc < 1, "io_submit");
	conn->state = STATE_ASYNC_ONGOING;
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	int rc;

	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");
	rc = close(conn->sockfd);
	DIE(rc < 0, "close");
	rc = close(conn->eventfd);
	DIE(rc < 0, "close");
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(struct sockaddr_in);
	/* TODO: Accept new connection. */
	int connfd = accept(listenfd, (SSA *) &addr, &addr_len);

	DIE(connfd < 0, "accept");
	/* TODO: Set socket to be non-blocking. */
	int flags = fcntl(connfd, F_GETFL, 0);

	DIE(flags < 0, "fcntl");
	flags |= O_NONBLOCK;
	int rc = fcntl(connfd, F_SETFL, flags);

	DIE(rc < 0, "fcntl");
	/* TODO: Instantiate new connection handler. */
	struct connection *conn = connection_create(connfd);
	/* TODO: Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, connfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");
	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	ssize_t bytes_recv;
	size_t pos = 0;

	while (1) {
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + pos, BUFSIZ, 0);
		if (bytes_recv <= 0)
			break;
		pos += bytes_recv;
	}
	conn->recv_len = pos;
	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	struct stat st;
	char *file_name, path_to_file[PATH_SIZE];
	int rc;

	if (conn->res_type == RESOURCE_TYPE_NONE)
		return -1;
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		// make file_name point to beginning of file_name in request_path
		file_name = strstr(conn->request_path, AWS_REL_STATIC_FOLDER) +
								 strlen(AWS_REL_STATIC_FOLDER);
		strcpy(conn->filename, file_name);
		strcpy(path_to_file, AWS_ABS_STATIC_FOLDER);
		strcat(path_to_file, conn->filename);
	} else {
		// here, resurce type is dynamic
		// make file_name point to beginning of file_name in request_path
		file_name = strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER) +
					strlen(AWS_REL_DYNAMIC_FOLDER);
		strcpy(conn->filename, file_name);
		strcpy(path_to_file, AWS_ABS_DYNAMIC_FOLDER);
		strcat(path_to_file, conn->filename);
	}
	conn->fd = open(path_to_file, O_RDONLY);
	DIE(conn->fd < 0, "open");
	// get file size to be able to create header before reading file
	rc = stat(path_to_file, &st);
	DIE(rc < 0, "stat");
	conn->file_size = st.st_size;
	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	int rc;

	// if it has read everything
	if (conn->file_pos == conn->file_size) {
		// cleanup
		// close file
		rc = close(conn->fd);
		DIE(rc < 0, "close file");
		rc = w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");
	}
	// conn->send_len += conn->file_pos;
	conn->state = STATE_SENDING_DATA;
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_out");
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	const http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	return http_parser_execute(&conn->request_parser, &settings_on_path,
							   conn->recv_buffer, conn->recv_len);
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	ssize_t bytes_sent;
	size_t len = conn->send_len, pos = 0;

	while (1) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + pos, len, 0);
		if (bytes_sent <= 0 || bytes_sent >= len)
			break;
		pos += bytes_sent;
		len -= bytes_sent;
	}
	// sendfile for sending the content of the file
	len = conn->file_size;
	while (1) {
		bytes_sent = sendfile(conn->sockfd, conn->fd, NULL, len);
		// problem caused by network congestion making sockfd fill up
		// its internal buffer => retry sendfile
		if (bytes_sent < 0 && errno == EWOULDBLOCK) {
			// reset errno to not get stuck in infinite loop
			errno = 0;
			continue;
		}
		if (bytes_sent <= 0 || bytes_sent >= len)
			break;
		len -= bytes_sent;
	}
	close(conn->fd);
	conn->state = STATE_DATA_SENT;
	connection_remove(conn);
	return STATE_NO_STATE;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	return -1;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	ssize_t bytes_sent, pos = 0;
	size_t len = conn->send_len;

	while (1) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + pos, len, 0);
		if (bytes_sent <= 0 || bytes_sent >= len)
			break;
		pos += bytes_sent;
		len -= bytes_sent;
	}
	// if it has not read everything from file
	if (conn->file_pos != conn->file_size) {
		// reset send buffer
		conn->send_pos = 0;
		conn->send_len = 0;
		// read more to file
		connection_start_async_io(conn);
		return 0;
	}
	// data sent successfully so close connection
	conn->state = STATE_DATA_SENT;
	connection_remove(conn);
	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	conn->state = STATE_RECEIVING_DATA;
	receive_data(conn);
	int rc;

	switch (conn->state) {
	case STATE_CONNECTION_CLOSED:
		return;
	case STATE_REQUEST_RECEIVED:
		rc = parse_header(conn);
		if (rc != conn->recv_len) {
			ERR("parse_header");
			connection_remove(conn);
			return;
		}
		conn->res_type = connection_get_resource_type(conn);
		if (conn->res_type == RESOURCE_TYPE_NONE) {
			connection_prepare_send_404(conn);
			// update socket for out events
			rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(rc < 0, "w_epoll_add_ptr_out");
			return;
		}
		// open file and get file size
		connection_open_file(conn);
		// create HTTP header
		connection_prepare_send_reply_header(conn);
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			// if resource is static, mark for sending
			conn->state = STATE_SENDING_DATA;
			rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(rc < 0, "w_epoll_update_ptr_out");
		} else {
			// gets here only if it is RESOURCE_TYPE_DYNAMIC
			// start async io for adding file data
			connection_start_async_io(conn);
		}
		break;
	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	ssize_t bytes_sent;

	switch (conn->state) {
	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		break;
	case STATE_SENDING_404:
		// send header in send_buffer
		while (1) {
			bytes_sent = send(conn->sockfd, conn->send_buffer, conn->send_len, 0);
			conn->send_len -= bytes_sent;
			if (bytes_sent <= 0 || conn->send_len <= 0)
				break;
		}
		conn->state = STATE_DATA_SENT;
		connection_remove(conn);
		break;
	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			connection_send_static(conn);
		} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			connection_send_dynamic(conn);
		} else {
			ERR("No connection resource type!");
			connection_remove(conn);
		}
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.<
	 */
	if (event & EPOLLIN)
		handle_input(conn);
	else if (event & EPOLLOUT)
		handle_output(conn);
	// connection_remove(conn);
}

int main(void)
{
	int rc;

	/* TODO: Initialize asynchronous operations. */
	rc = io_setup(10000, &ctx);
	DIE(rc != 0, "io_setup");
	/* TODO: Initialize multiplexing. */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");
	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");
	/* TODO: Add server socket to epoll object*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");
	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");
		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			// new connection
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, rev.data.ptr);
		}
	}

	return 0;
}
