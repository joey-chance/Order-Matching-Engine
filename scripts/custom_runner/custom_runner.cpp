#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/un.h>
#include <sys/socket.h>

#include <atomic>
#include <fstream>

#include "io.hpp"

#define INPUT_CANCEL_ORDER 'C'
#define INPUT_BUY_ORDER 'B'
#define INPUT_SELL_ORDER 'S'

static char* line_buffer;
static size_t line_buffer_size = 0;
static std::atomic<bool> main_is_exiting = 0;

static void* poll_thread(void* fdptr)
{
	struct pollfd pfd {};
	pfd.fd = (int) (long) fdptr;
	pfd.events = 0;

	while(!main_is_exiting)
	{
		if(poll(&pfd, 1, -1) == -1)
		{
			perror("poll");
			_exit(1);
		}
		if(main_is_exiting)
		{
			break;
		}
		if(pfd.revents & (POLLERR | POLLHUP))
		{
			fprintf(stderr, "Connection closed by server\n");
			_exit(0);
		}
	}
	return 0;
}

static void* run_client(void *sock_path)
{
    // Get char * from void*
    char *socket_path = (char*)sock_path;

	//Default pthread_exit values
    void *exit_0 = 0;
    void *exit_1 = (void*) 1;


    //Create conn to engine
	int clientfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(clientfd == -1)
	{
		perror("socket");
		pthread_exit(exit_1);//return 1;
	}

	{
		struct sockaddr_un sockaddr {};
		sockaddr.sun_family = AF_UNIX;
		strncpy(sockaddr.sun_path, socket_path, sizeof(sockaddr.sun_path) - 1);
		if(connect(clientfd, (const struct sockaddr*) &sockaddr, sizeof(sockaddr)) != 0)
		{
			perror("connect");
			pthread_exit(exit_1);//return 1;
		}
	}
    //Create conn to engine END

    //Sets client fd to (socket to engine)
	FILE* client = fdopen(clientfd, "r+");
	setbuf(client, NULL); //Used for poll_thread, polls engine
    
	pthread_t poll_thread_handle;
	if(pthread_create(&poll_thread_handle, NULL, poll_thread, (void*) (long) clientfd) < 0)
	{
		fprintf(stderr, "Failed to create poll thread\n");
		pthread_exit(exit_1);//return 1;
	}

    FILE *input_file = fopen("c1.in", "r");
	while(1)
	{
		ClientCommand input {};
        // Read from the text file
        // std::ifstream MyReadFile("c1.in");
        
		ssize_t line_length = getline(&line_buffer, &line_buffer_size, input_file);
		if(line_length == -1)
			break;

		switch(line_buffer[0])
		{
			case '#':
			case '\n': continue;
			case INPUT_CANCEL_ORDER:
				input.type = input_cancel;
				if(sscanf(line_buffer + 1, " %u", &input.order_id) != 1)
				{
					fprintf(stderr, "Invalid cancel order: %s\n", line_buffer);
					pthread_exit(exit_1);//return 1;
				}
				break;
			case INPUT_BUY_ORDER: input.type = input_buy; goto new_order;
			case INPUT_SELL_ORDER:
				input.type = input_sell;
			new_order:
				if(sscanf(line_buffer + 1, " %u %8s %u %u", &input.order_id, input.instrument, &input.price, &input.count) != 4)
				{
					fprintf(stderr, "Invalid new order: %s\n", line_buffer);
					pthread_exit(exit_1);//return 1;
				}
				break;
			default: fprintf(stderr, "Invalid command '%c'\n", line_buffer[0]); pthread_exit(exit_1);//return 1;
		}

		if(fwrite(&input, 1, sizeof(input), client) != sizeof(input))
		{
			fprintf(stderr, "Failed to write command\n");
			pthread_exit(exit_1);
		}
	}

	main_is_exiting = 1;
	fclose(client);

	pthread_exit(ferror(stderr) ? exit_1 : exit_0);//return ferror(stderr) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    //argv[1] should be socket path
    //pthread_create(run_client, argv[1])
    if(argc < 2)
	{
		fprintf(stderr, "Usage: %s <path of socket to connect to> < <input>\n", argv[0]);
		return 1;
	}
    //Get Socket path
    char sock_path[100];
    strncpy(sock_path, argv[1], strlen(argv[1]) + 1);
    // fprintf(stdout, "%s\n", argv[1]);
    // fprintf(stdout, "%s end\n", sock_path);
    // Get number of input files, N_Clients
    // argc -2
    // print error if less than 1
    int N_Clients = argc - 2;
    if(N_Clients < 1)
    {
        fprintf(stderr, "Need at least 1 client input file");
        return 1;
    }
    //Create array of client threads and statuses, size of N_Clients
    //Loops through and run create/join
    const int max_threads = 40;
    pthread_t client_threads[max_threads];
    void *client_statuses[max_threads];
    for (int i=0; i< N_Clients; i++) {
        pthread_t *client_thread = &client_threads[i];
        void *client_status = client_statuses[i];

        pthread_create(client_thread, NULL, run_client, (void*) sock_path);
        pthread_join(*client_thread, &client_status);
    }
    
}
