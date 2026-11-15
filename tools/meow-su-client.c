/* Explicit client for the boot-scoped MeowSU service. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define COMMAND_LIMIT 16384
#define FRAME_LIMIT 8192
#define MAGIC 0x4d535531

static int transfer(int fd, void *data, size_t length, int sending)
{
	char *cursor = data;

	while (length > 0) {
		ssize_t count = sending ? send(fd, cursor, length, MSG_NOSIGNAL)
					: read(fd, cursor, length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count == 0)
			errno = EPIPE;
		if (count <= 0)
			return -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 0;
}

static void *forward_stdin(void *argument)
{
	int fd = (int)(intptr_t)argument;
	char buffer[FRAME_LIMIT];

	for (;;) {
		ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0 || transfer(fd, buffer, (size_t)count, 1) < 0)
			break;
	}
	shutdown(fd, SHUT_WR);
	return NULL;
}

static int run_command(int fd, char *command, size_t length)
{
	uint32_t header[] = { htonl(MAGIC), htonl((uint32_t)length) };
	uint32_t frame;
	char buffer[FRAME_LIMIT];
	pthread_t input;
	int error;

	if (transfer(fd, &frame, sizeof(frame), 0) < 0)
		return -1;
	if (frame != 0) {
		fputs("MeowSU: this app is not authorized for this boot\n", stderr);
		return 126;
	}
	if (transfer(fd, header, sizeof(header), 1) < 0 ||
	    transfer(fd, command, length, 1) < 0)
		return -1;
	error = pthread_create(&input, NULL, forward_stdin, (void *)(intptr_t)fd);
	if (error != 0) {
		errno = error;
		return -1;
	}
	pthread_detach(input);
	for (;;) {
		if (transfer(fd, &frame, sizeof(frame), 0) < 0)
			return -1;
		frame = ntohl(frame);
		if (frame == UINT32_MAX) {
			if (transfer(fd, &frame, sizeof(frame), 0) < 0)
				return -1;
			frame = ntohl(frame);
			return frame <= 255 ? (int)frame : 125;
		}
		if (frame == 0 || frame > sizeof(buffer)) {
			errno = EPROTO;
			return -1;
		}
		if (transfer(fd, buffer, frame, 0) < 0 ||
		    fwrite(buffer, 1, frame, stdout) != frame || fflush(stdout) != 0)
			return -1;
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX,
		.sun_path = "/dev/.meow-manager/su" };
	struct ucred peer;
	socklen_t peer_size = sizeof(peer);
	size_t length;
	char *command = NULL;
	int first = 1;
	int fd, result = 125;

	if (first < argc && (!strcmp(argv[first], "0") || !strcmp(argv[first], "root")))
		first++;
	if (argc - first == 2 && !strcmp(argv[first], "-c")) {
		command = argv[first + 1];
	} else if (argc - first != 0) {
		fputs("Usage: su [root|0] [-c COMMAND]\n", stderr);
		return 2;
	}
	length = command ? strlen(command) : 0;
	if ((command && length == 0) || length > COMMAND_LIMIT) {
		fputs("MeowSU: invalid command length\n", stderr);
		return 2;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("MeowSU: socket");
		return result;
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("MeowSU: connect");
	} else if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 ||
		   peer_size != sizeof(peer) || peer.uid != 0) {
		fputs("MeowSU: invalid service identity\n", stderr);
	} else {
		result = run_command(fd, command, length);
		if (result < 0) {
			perror("MeowSU: request");
			result = 125;
		}
	}
	close(fd);
	return result;
}
