#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <signal.h>
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

#define QUEUE_DEPTH 2048
#define BUF_SIZE 100

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
	int64_t timestamp;
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
	// Ignore SIGPIPE signal
	signal(SIGPIPE, SIG_IGN);

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
		bool seen_cqe = false;

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
					ptr->timestamp = t1.time_since_epoch().count();

					ptr->iov.iov_base = &ptr->timestamp;	   // Remove ptr->buffer assignment
					ptr->iov.iov_len = sizeof(ptr->timestamp); // Update iov_len

					size_t timestamp_size = sizeof(int64_t);
					std::cout << "Sending timestamp with " << timestamp_size << " bytes" << std::endl;

					io_uring_prep_writev(sqe, fd, &ptr->iov, 1, 0);
					io_uring_submit(&ring); // submit prepared SQE to the io_uring
					continue;
				}
			}


			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret < 0)
			{
				perror("io_uring_wait_cqe");
				break;
			}
		}

		my_completion_data_t *completion = (my_completion_data_t *)io_uring_cqe_get_data(cqe);

		// only process the completion event if completion is not a null pointer
		if (completion)
		{
			if (cqe->res < 0)
			{
				// Error handling, logging the error message
				std::string error_message = "Async operation failed, error: ";
				error_message += strerror(-cqe->res);
				std::cerr << "[Error] " << error_message << std::endl;

				if (-cqe->res == EPIPE)
				{
					switch (completion->type)
					{
					case Types::EchoCompletion:
					{
						echo_write_completion_t *echo_ptr = static_cast<echo_write_completion_t *>(completion);
						printf("Client disconnected (fd %d)\n", echo_ptr->fd);

						// Remove the client's file descriptor from your active_fds set
						active_fds.erase(echo_ptr->fd);

						// Close the socket and remove the associated resources
						close(echo_ptr->fd);
					}
					break;
					case Types::TimeWriteCompletion:
					{
						time_write_completion_t *write_ptr = static_cast<time_write_completion_t *>(completion);
						printf("Client disconnected (fd %d)\n", write_ptr->fd);

						// Remove the client's file descriptor from your active_fds set
						active_fds.erase(write_ptr->fd);

						// Remove the client's file descriptor from your time_write_fds map
						time_write_fds.erase(write_ptr->fd);

						// Close the socket
						close(write_ptr->fd);

						seen_cqe = true;
					}
					break;
					default:
						break;
					}
				}

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
					if (bytes <= 0)
					{
						if (cqe->res == -ECONNRESET)
						{
							printf("Client disconnected (fd %d)\n", read_ptr->fd);
						}
						else
						{
							printf("Unknown error (fd %d)\n", read_ptr->fd);
						}
						active_fds.erase(read_ptr->fd);
						close(read_ptr->fd);
						seen_cqe = true;
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
					if (bytes <= 0)
					{
						if (cqe->res == -ECONNRESET)
						{
							printf("Client disconnected (fd %d)\n", read_ptr->fd);
						}
						else
						{
							printf("Unknown error (fd %d)\n", read_ptr->fd);
						}
						active_fds.erase(read_ptr->fd);
						close(read_ptr->fd);
						delete read_ptr;
						seen_cqe = true;
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

				active_fds.emplace(fd);

				time_write_fds.emplace(fd, socket_data_t{fd, 0});

				io_uring_sqe *next_sqe = io_uring_get_sqe(&ring);
				io_uring_prep_accept(next_sqe, listen_fd, nullptr, nullptr, 0);
				io_uring_sqe_set_data(next_sqe, &accept_completion);
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
						// printf("reduced writes in progress to (%d) (fd %d)\n", it->second.writes_in_progress, it->second.fd);
					}
					delete write_ptr;
				}
			}
			break;
			}

			if (!completion)
			{
				io_uring_cqe_seen(&ring, cqe);
			}
		}
	}

	io_uring_queue_exit(&ring);
	close(listen_fd);
	return 0;
}