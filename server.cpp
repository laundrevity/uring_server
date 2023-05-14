#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <chrono>
#include <charconv>

#include <unordered_set>
#include <unordered_map>
#include <system_error>
#include <iostream>

#define QUEUE_DEPTH 32
#define BUF_SIZE 4096

enum class Types
{
	ReadCompletion,
	EchoCompletion,
	AcceptCompletion,
	TimeWriteCompletion
};

struct my_completion_data_t
{
	virtual ~my_completion_data_t() = default;
	Types type;
};

struct read_completion_t : my_completion_data_t
{
	read_completion_t()
	{
		this->type = Types::ReadCompletion;
	}
	int fd;
	iovec iov;
	char buffer[BUF_SIZE];
	int write_in_progress;
};

struct accept_completion_t : my_completion_data_t
{
	accept_completion_t()
	{
		this->type = Types::AcceptCompletion;
	}
};
struct echo_write_completion_t : my_completion_data_t
{
	echo_write_completion_t()
	{
		this->type = Types::EchoCompletion;
	}
	int fd;
};

struct time_write_completion_t : my_completion_data_t
{
	time_write_completion_t()
	{
		this->type = Types::TimeWriteCompletion;
	}
	iovec iov;
	char buffer[BUF_SIZE];
	int fd;
	int writes_in_progress; // delete when 0
};

struct socket_data_t
{
	int fd;
	int writes_in_progress;
};

static int set_socket_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
	{
		perror("fcntl");
		return -1;
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1)
	{
		perror("fcntl");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s <port>\n", argv[0]);
		return 1;
	}

	int queue_pressure = 5;
	if (argc > 2)
	{
		queue_pressure = atoi(argv[1]);
		if (queue_pressure <= 0 || queue_pressure > 65535)
		{
			fprintf(stderr, "Invalid queue_pressure\n");
			return 1;
		}
	}

	int port = atoi(argv[1]);
	if (port <= 0 || port > 65535)
	{
		fprintf(stderr, "Invalid port number\n");
		return 1;
	}

	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
	{
		perror("socket");
		return 1;
	}

	if (set_socket_nonblocking(listen_fd) < 0)
	{
		close(listen_fd);
		return 1;
	}

	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	int enable = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
	{
		perror("setsockopt");
		close(listen_fd);
		return 1;
	}

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		perror("bind");
		close(listen_fd);
		return 1;
	}

	if (listen(listen_fd, SOMAXCONN) < 0)
	{
		perror("listen");
		close(listen_fd);
		return 1;
	}

	printf("Listening on port %d\n", port);

	io_uring ring;
	if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0)
	{
		perror("io_uring_queue_init");
		close(listen_fd);
		return 1;
	}

	accept_completion_t accept_completion{};

	io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);

	io_uring_sqe_set_data(sqe, &accept_completion);

	io_uring_submit(&ring);

	std::unordered_set<int> active_fds;
	std::unordered_map<int, socket_data_t> time_write_fds;

	for (;;)
	{
		io_uring_cqe *cqe;
		int ret = io_uring_peek_cqe(&ring, &cqe);
		if (ret < 0 && ret != -EAGAIN)
		{
			perror("io_uring_peek_cqe");
			break;
		}
		else if (ret == -EAGAIN)
		{
			// nothing to do, try to send timestamps again to fill up the queue, otherwise. just wait
			auto t1 = std::chrono::steady_clock::now();
			bool sent = false;
			for (auto &[fd, socket_data] : time_write_fds)
			{
				if (socket_data.writes_in_progress < queue_pressure)
				{
					sent = true;
					io_uring_sqe *sqe = io_uring_get_sqe(&ring);
					time_write_completion_t *ptr = new time_write_completion_t{};
					ptr->writes_in_progress = 1;
					ptr->fd = fd;

					if (auto [end_ptr, ec] = std::to_chars(ptr->buffer, ptr->buffer + BUF_SIZE, t1.time_since_epoch().count());
						ec == std::errc())
					{
						// all good
						ptr->iov.iov_base = ptr->buffer;
						size_t size = strlen("\n");
						strncpy(end_ptr, "\n", size);
						end_ptr += size;
						ptr->iov.iov_len = end_ptr - ptr->buffer;
						io_uring_prep_writev(sqe, fd, &ptr->iov, 1, 0);

						io_uring_sqe_set_data(sqe, ptr);

						++socket_data.writes_in_progress;
						printf("increased writes in progress to (%d) (fd %d)\n", socket_data.writes_in_progress, socket_data.fd);
						io_uring_submit(&ring);
					}
					else
					{
						std::cout << std::make_error_code(ec).message() << '\n';
					}
				}
			}

			if (sent)
			{
				continue;
			}
			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret < 0)
			{
				perror("io_uring_wait_cqe");
				break;
			}
		}

		my_completion_data_t *completion = (my_completion_data_t *)io_uring_cqe_get_data(cqe);
		if (cqe->res < 0)
		{
			fprintf(stderr, "Async operation failed: %s\n", strerror(-cqe->res));
			io_uring_cqe_seen(&ring, cqe);
			continue;
		}

		switch (completion->type)
		{
		case Types::ReadCompletion:
		{
			read_completion_t *read_ptr = static_cast<read_completion_t *>(completion);
			// This is a read or write event
			if (!read_ptr->write_in_progress)
			{
				// read
				size_t bytes = cqe->res;
				if (bytes == 0)
				{
					// Connection closed
					printf("Connection closed (fd %d)\n", read_ptr->fd);
					active_fds.erase(read_ptr->fd);
					close(read_ptr->fd);
					delete read_ptr;
				}
				else
				{
					printf("read (fd %d)\n", read_ptr->fd);

					read_ptr->write_in_progress = 1;
					// Echo the received data
					for (const int fd : active_fds)
					{
						if (fd == read_ptr->fd)
						{
							continue;
						}
						io_uring_sqe *sqe = io_uring_get_sqe(&ring);
						io_uring_prep_writev(sqe, fd, &read_ptr->iov, 1, 0);
						echo_write_completion_t *echo_ptr = new echo_write_completion_t;
						echo_ptr->fd = fd;
						io_uring_sqe_set_data(sqe, echo_ptr);

						io_uring_submit(&ring);
					}
					io_uring_sqe *sqe = io_uring_get_sqe(&ring);
					io_uring_prep_writev(sqe, read_ptr->fd, &read_ptr->iov, 1, 0);
					io_uring_sqe_set_data(sqe, read_ptr);

					io_uring_submit(&ring);
				}
			}
			else
			{
				// write
				size_t bytes = cqe->res;
				if (bytes == 0)
				{
					// Connection closed
					printf("Connection closed (fd %d)\n", read_ptr->fd);
					active_fds.erase(read_ptr->fd);
					close(read_ptr->fd);
					delete read_ptr;
				}
				else
				{
					printf("write completed (fd %d)\n", read_ptr->fd);
					// setup new read
					read_ptr->write_in_progress = 0;
					io_uring_sqe *sqe = io_uring_get_sqe(&ring);
					io_uring_prep_readv(sqe, read_ptr->fd, &read_ptr->iov, 1, 0);
					io_uring_sqe_set_data(sqe, read_ptr);
					io_uring_submit(&ring);
				}
			}
		}
		break;
		case Types::EchoCompletion:
		{
			echo_write_completion_t *echo_ptr = (echo_write_completion_t *)completion;
			delete echo_ptr;
		}
		break;
		case Types::AcceptCompletion:
		{
			// This is an accept event
			int fd = cqe->res;
			printf("Accepted connection (fd %d)\n", fd);

			if (set_socket_nonblocking(fd) < 0)
			{
				close(fd);
				break;
			}

			read_completion_t *read_ptr = new read_completion_t;
			read_ptr->iov.iov_base = read_ptr->buffer;
			read_ptr->iov.iov_len = BUF_SIZE;
			read_ptr->fd = fd;

			struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
			io_uring_prep_readv(sqe, read_ptr->fd, &read_ptr->iov, 1, 0);
			io_uring_sqe_set_data(sqe, read_ptr);

			active_fds.emplace(read_ptr->fd);
			auto &time_write = time_write_fds.try_emplace(fd).first->second;
			time_write.fd = fd;

			// Prepare for the next connection
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);

			io_uring_sqe_set_data(sqe, &accept_completion);

			io_uring_submit(&ring);
		}
		break;
		case Types::TimeWriteCompletion:
		{
			time_write_completion_t *write_ptr = static_cast<time_write_completion_t *>(completion);
			// write completed
			--write_ptr->writes_in_progress;
			if (!write_ptr->writes_in_progress)
			{
				auto it = time_write_fds.find(write_ptr->fd);
				if (it != time_write_fds.end())
				{
					--it->second.writes_in_progress;
					printf("reduced writes in progress to (%d) (fd %d)\n", it->second.writes_in_progress, it->second.fd);
				}
				delete write_ptr;
			}
		}
		break;
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	close(listen_fd);
	return 0;
}