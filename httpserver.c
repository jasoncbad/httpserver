#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>

// macros + enum type
#define BUFFER_SIZE 4096
#define RESPONSE_HEADER_SIZE 256
#define DEFAULT_POOL_SIZE 4
enum command_t{PUT, GET, HEAD};

// Prototypes
static uint16_t put_executor(int client_sockd, char *res_name, uint8_t *buff,
	size_t content_length, size_t bytes_waiting, size_t body_starts);
static uint16_t get_executor(int client_sockd, char *res_name, uint8_t *buff);
static uint16_t head_executor(int client_sockd, char *res_name);
static uint16_t handle_error(int client_sockd, uint16_t code);
static bool validate_resource(char *res_name);
static void build_message(char* msg, uint16_t code, size_t content_length);
static ssize_t recv_full(int fd, uint8_t *buff, ssize_t size);
static bool isError(uint16_t code);

struct assignment {
	int assignment_fd;
	struct assignment* next;
};

struct log_request {
	enum command_t command;
	char* res_name;
	size_t content_length;
	uint16_t status_code;
	char* first_line;
	char* health_check_content;
	struct log_request* next;
};

struct thread_args {
	int thread_id;
	bool logging;
	char* log_file;
	int* num_assignments;
	int* num_pending_logs;
	size_t* total_requests;
	size_t* total_errors;
	pthread_mutex_t* assignment_mutex;
	pthread_mutex_t* log_mutex;
	pthread_mutex_t* server_health_mutex;
	pthread_cond_t* got_log;
	pthread_cond_t* got_assignment;
};

struct logger_args {
	int log_fd;
	char* log_file;
	pthread_mutex_t* log_mutex;
	pthread_cond_t* got_log;
	int* num_pending_logs;
};

struct assignment* assignments = NULL;
struct assignment* last_assignment = NULL;
struct log_request* log_requests = NULL;
struct log_request* last_log_request = NULL;

// prototypes added to this server in the multithreading-gen functions
void* worker_thread(void* data);
void* logger_thread(void* data);
void add_assignment(int client_fd, pthread_mutex_t* p_mutex, pthread_cond_t* p_cond_var, int* num_assignments);
struct assignment* get_assignment(int* num_assignments);
void pend_log(struct log_request* a_log_request, pthread_mutex_t* p_mutex, pthread_cond_t* p_cond_var, int* num_pending_logs);
struct log_request* get_pending_log(int* num_pending_logs);
struct log_request* perform_work(struct assignment* an_assignment, int thread_id, char* log_file, size_t* total_requests, size_t* total_errors, bool logging, pthread_mutex_t* server_health_mutex);

// strings used often
const char HTTP[] = "HTTP/1.1";
const char CODE_200[] = "200 OK";
const char CODE_201[] = "201 Created";
const char CODE_400[] = "400 Bad Request";
const char CODE_403[] = "403 Forbidden";
const char CODE_404[] = "404 Not Found";
const char CODE_500[] = "500 Internal Server Error";
const char CONTENT_LENGTH[] = "Content-Length:";
const char DCRLF[] = "\r\n\r\n";

//------------------------------------------------------------------------------
//     MAIN | SETUP, LISTEN, and DISPATCH
//------------------------------------------------------------------------------
int main(int argc, char *argv[]) {

	// variable initialization
	pthread_mutex_t assignment_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t server_health_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t got_assignment = PTHREAD_COND_INITIALIZER;
	pthread_cond_t got_log = PTHREAD_COND_INITIALIZER;
	int num_assignments = 0;
	int num_pending_logs = 0;
	size_t total_requests = 0;
	size_t total_errors = 0;
	size_t num_threads = DEFAULT_POOL_SIZE;
	bool logging = 0;
	char log_file[100];
	int port = 0;
	int opt;

	// parse arguments
	while (1) {
		opt = getopt(argc, argv, "N:l:");
		if (opt == -1) { break; }
		if (opt == 'N') {
			if (atoi(optarg) > 0) {
				num_threads = atoi(optarg);
			} else {
				printf("Invalid flag argument for -N\n");
				return 1;
			}
		} else if (opt == 'l') {
			strcpy(log_file, optarg);
			logging = 1;
		} else {
			break;
		}
	}
	if (optind != argc) {
		if (atoi(argv[optind]) > 0) {
			port = atoi(argv[optind]);
		} else {
			printf("Invalid port!\n");
			return 1;
		}
	} else {
		printf("Invalid port!\n");
		return 1;
	}

	// build arguments for thread initialization
	struct thread_args* t_args;
	t_args = (struct thread_args*)malloc(sizeof(struct thread_args));
	t_args->thread_id = -1;
	t_args->logging = logging;
	t_args->log_file = log_file; // might need to copy into?
	t_args->num_assignments = &num_assignments; // shared variable
	t_args->num_pending_logs = &num_pending_logs; // shared variable
	t_args->total_requests = &total_requests; // shared
	t_args->total_errors = &total_errors; // shared
	t_args->assignment_mutex = &assignment_mutex; // shared mutex
	t_args->log_mutex = &log_mutex; // shared mutex
	t_args->server_health_mutex = &server_health_mutex;
	t_args->got_log = &got_log; // shared conditional
	t_args->got_assignment = &got_assignment;

	struct logger_args* l_args = NULL;
	if (logging) {
		l_args = (struct logger_args*)malloc(sizeof(struct logger_args));
		l_args->log_file = log_file;
		l_args->log_mutex = &log_mutex;
		l_args->got_log = &got_log;
		l_args->num_pending_logs = &num_pending_logs;

		l_args->log_fd = open(log_file, O_CREAT|O_WRONLY|O_TRUNC, 0644);

		if (l_args->log_fd == -1) {
			printf("Error creating/opening log file! Logger thread init aborted...\n");
			logging = 0;
			free(l_args);
		}
	}

	// spin up threads
	pthread_t p_threads[num_threads];
	for (int i = 0; i < (ssize_t)num_threads; i++) {
		t_args->thread_id = i;
		pthread_create(&p_threads[i], NULL, worker_thread, (void*)t_args);
	}

	if (logging) {
		pthread_t logger_pthread;
		pthread_create(&logger_pthread, NULL, logger_thread, (void*)l_args);
	}

  // setting up socket for listening
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t addrlen = sizeof(server_addr);
  int server_sockd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sockd < 0) {
      perror("socket");
  }
  int enable = 1;
  int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

  struct sockaddr client_addr;
  socklen_t client_addrlen = sizeof(client_addr);

  ret = listen(server_sockd, 5);

	if (ret == -1) {
		printf("[+] issue with socket set up!\n");
		return 1;
	}

	// listen and dispatch
	while (1) {
		printf("[+] dispatcher waiting...\n");
		int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
		if (client_sockd == -1) {
			printf("  [+] error accepting connection!");
			continue;
		}

		// creatre assignment
		add_assignment(client_sockd, &assignment_mutex, &got_assignment, &num_assignments);
		printf("[+] dispatcher created an assignment...\n");
	}
	return 1;
}

//------------------------------------------------------------------------------
//   REQUEST EXECUTION FUNCTIONS
//------------------------------------------------------------------------------

/*
	EXECUTION STAGE: put_executor()
	Carries out a PUT request.

	PRE-CONDITIONS: The client socket is open and ready to recieve a message.
		The resource name is validated before calling this function. The buffer is
		empty waiting for use.
	POST-CONDITIONS: The HTTP response is sent to the client. 200 is returned for
		OK. If an error occurs, the HTTP error code is returned for use by the
		handle_error() function.
*/
static uint16_t put_executor(int client_sockd, char *res_name, uint8_t *buff,
size_t content_length, size_t bytes_waiting, size_t body_start) {
	printf("    [+] put_executor() called... \n\t\tres: %s\n\t\tcontent-length: %ld\n\t\tbody starts at: (buff + %ld)\n\t\tbytes waiting in buffer before call: %ld\n", res_name, content_length, body_start, bytes_waiting);

	// set up
	size_t bytes_written = 0;
	ssize_t recv_status = 0;
	ssize_t write_status = 0;
	int resource_fd = -1;

	// use stat to check for 403, 404, 500
	struct stat st;
	if ( !(stat(res_name, &st) == -1) ) {
			if (!(st.st_mode & S_IWUSR)) {
	 			return 403;
	 		}
	}

	resource_fd = open(res_name, O_CREAT|O_WRONLY|O_TRUNC, 0644);

	if (resource_fd == -1) {
		printf("      [+] problem with resourceFD!\n");
		if (errno == ENOENT) {
			return 404;
		}
		return 500;
	}

	// write any bytes that were waiting before this function call
	if (bytes_waiting != 0) {
		uint8_t *start = (buff + body_start);
		write_status = write(resource_fd, start, bytes_waiting);
	}

	if (write_status  == -1) {
			printf("      [+] problem with initial write!\n");
			return 500;
	}

	bytes_written += write_status;

	// perform PUT
	while (bytes_written < content_length) {
		recv_status = recv(client_sockd, buff, BUFFER_SIZE, 0);

		if (recv_status == -1 || recv_status == 0) {
			printf("      [+] problem with recv!\n");
			return 500;
		}

		write_status = write(resource_fd, buff, recv_status);

		if (write_status  == -1) {
			printf("      [+] problem with write!\n");
			return 500;
		}

		bytes_written += recv_status;
	}

	// send response and close connection
	char response[RESPONSE_HEADER_SIZE] = "";
	build_message(response, 201, 0);
	send(client_sockd, response, strlen(response), 0);
	close(resource_fd);
	printf("    [+] PUT finished 201: %ld bytes transferred!\n", bytes_written);
	return 201;
}


/*
	EXECUTION STAGE: get_executor()
	Carries out a GET request.

	PRE-CONDITIONS: The client socket is open and ready to recieve a message/data.
		The resource name is validated before calling this function. The buffer is
		empty waiting for use.
	POST-CONDITIONS: The HTTP response is sent to the client. 200 is returned for
		OK. If an error occurs, the HTTP error code is returned for use by the
		handle_error() function.
*/
static uint16_t get_executor(int client_sockd, char *res_name, uint8_t *buff) {
	printf("    [+] get_executor() called...\n\t\tres: %s\n", res_name);

	size_t bytes_sent = 0;
	ssize_t read_status = 0;
	ssize_t send_status = 0;
	off_t content_length = 0;
	int resource_fd = -1;

	// use stat to check for 403, 404, 500
	struct stat st;
	if ( !(stat(res_name, &st) == -1) ) {
		if (!(st.st_mode & S_IRUSR)) {
 			return 403;
 		}
		content_length = st.st_size;
	}

	resource_fd = open(res_name, O_RDONLY);

	if (resource_fd == -1) {
		printf("      [+] problem with resourceFD!\n");
		if (errno == ENOENT) {
			return 404;
		}
		return 500;
	}

	// send out the response
	char response[RESPONSE_HEADER_SIZE] = "";
	build_message(response, 200, content_length);
	send(client_sockd, response, strlen(response), 0);

	// perform GET
	read_status = read(resource_fd, buff, BUFFER_SIZE);

	if (read_status == -1) {
		printf("      [+] problem with reading! (1)\n");
		return 500;
	}

	while (read_status > 0) {

		if (read_status == -1) {
			printf("      [+] problem with reading! (2)\n");
			return 500;
		}

		send_status = send(client_sockd, buff, read_status, 0);

		if (send_status == -1) {
			printf("      [+] problem with sending! (1)\n");
			return 500;
		}

		bytes_sent += send_status;

		read_status = read(resource_fd, buff, BUFFER_SIZE);

		if (read_status == -1) {
			printf("      [+] problem with reading! (2)\n");
			return 500;
		}

	}

	close(client_sockd);
	printf("    [+] GET finished 200: %ld bytes transferred!\n", bytes_sent);
	return 200;
}


/*
	EXECUTION STAGE: head_executor()
	Carries out a HEAD request.

	PRE-CONDITIONS: The client socket is open and ready to recieve a message.
		The resource name is validated before calling this function.
	POST-CONDITIONS: The HTTP response is sent to the client. 200 is returned for
		OK. If an error occurs, the HTTP error code is returned for use by the
		handle_error() function.
*/
static uint16_t head_executor(int client_sockd, char *res_name) {
	printf("    [+] head_executor() called...\n\t\tres: %s\n", res_name);

	off_t content_length = 0;
	int resource_fd;

	// use stat to check for 403, 404, 500
	struct stat st;
	if ( !(stat(res_name, &st) == -1) ) {
		if (!(st.st_mode & S_IRUSR)) {
 			return 403;
 		}
		content_length = st.st_size;
	}

	resource_fd = open(res_name, O_RDONLY);

	if (resource_fd == -1) {
		printf("      [+] problem with resourceFD!\n");
		if (errno == ENOENT) {
			return 404;
		}
		return 500;
	}

	// send out the response
	char response[RESPONSE_HEADER_SIZE] = "";
	build_message(response, 200, content_length);
	send(client_sockd, response, strlen(response), 0);
	close(client_sockd);
	printf("    [+] HEAD finished 200: 0 bytes transferred!\n");
	return 200;
}

//------------------------------------------------------------------------------
//   HELPER FUNCTIONS FOR FULFULLING REQUESTS
//------------------------------------------------------------------------------
/*
	ERROR HANDLING STAGE: handle_error()
	Sends any error messages to the client socket when called. 200 codes are
		ignored.

	PRE-CONDITIONS: The client socket is open and ready for a message. code is
		one of the valid HTTP response codes for this assignment.
	POST-CONDITIONS: The message is built using build_message() and sent to the
		client. Connection is then closed.
*/
static uint16_t handle_error(int client_sockd, uint16_t code) {
	if (code == 200 || code == 201)
		return code;

	printf("    [+] %d thrown!\n", code);

	char response[RESPONSE_HEADER_SIZE] = "";
	build_message(response, code, 0);

	send(client_sockd, response, strlen(response), 0);
	close(client_sockd);
	return code;
}

/*
	RECEIVING: recv_full()

	Used when calling first receiving data from the accepted connection.
	Does not stop receiving until a full header is received.

	THIS IS CODE FROM CLARK's SECTION
	It is slightly modified for my program: buff is now uint8_t,
		and a check for \r\n\r\n
*/
static ssize_t recv_full(int fd, uint8_t *buff, ssize_t size) {
	ssize_t total = 0;

	while (total < size) {
		ssize_t ret = recv(fd, buff+total, size-total, 0);

		if (ret < 0) {
			// no data right now, try again
			if (errno = EAGAIN) {
				continue;
			}
			return ret;
		}	else if (ret == 0) {
			// stream socket peer performed an orderly shutdown
			return total;
		} else {
			total += ret;
			// do we have a full HTTP header yet?
			if (strstr((char*)buff, DCRLF) != NULL) {
				return total;
			}
		}
	}
	return total;
}

/*
	HELPER FUNCTION: validate_resource()
	validates the token corresponding to the target resource

	PRE-CONDITIONS: res_name can be any string, empty or not.
	POST-CONDITIONS: 0 or 1 is returned for a invalid or valid
		resource respectively. Valid resource name is provided by the asgn1 spec.
*/
static bool validate_resource(char * res_name) {
	if (*(res_name) != '/') {
		return 0;
	}

	if (strlen(res_name) > 28 || strlen(res_name) < 2) {
		return 0;
	}

	char *cursor = (res_name + 1);

	while (*(cursor) != '\0') {
		char current_char = *cursor;

		if (!( (current_char > 47 && current_char < 58) ||
			(current_char > 64 && current_char < 91) ||
			(current_char > 96 && current_char < 123) ||
			(current_char == 45) || (current_char == 95) )) {
			return 0;
		}
		cursor = cursor + 1;
	}
	return 1;
}

/*
	HELPER FUNCTION: build_message()
	Builds an http response in a given msg string using the code and
	content_length.

	PRE-CONDITIONS: msg is an empty string, code is one of the HTTP status codes
			and content_length is greater than or equal to 0.
	POST-CONDITIONS: msg contains the response header ready for sending.
*/
static void build_message(char* msg, uint16_t code, size_t content_length) {
	strcat(msg, HTTP); strcat(msg, " ");

	switch(code) {
		case 200:
			strcat(msg, CODE_200);
			break;
		case 201:
			strcat(msg, CODE_201);
			break;
		case 400:
			strcat(msg, CODE_400);
			break;
		case 403:
			strcat(msg, CODE_403);
			break;
		case 404:
			strcat(msg, CODE_404);
			break;
		case 500:
			strcat(msg, CODE_500);
			break;
	}

	strcat(msg, "\r\n");
	strcat(msg, CONTENT_LENGTH); strcat(msg, " ");

	char file_size[10]; // more than resonable
	sprintf(file_size, "%ld", content_length);

	strcat(msg, file_size); strcat(msg, "\r\n\r\n");
}

static bool isError(uint16_t code) {
	switch (code) {
		case 400:
		case 403:
		case 404:
		case 500:
		return 1;
	}
	return 0;
}

//------------------------------------------------------------------------------
//   WORKER AND LOGGER THREADS
//------------------------------------------------------------------------------


void* worker_thread(void* data) {
  int rc;
  struct assignment* an_assignment;

	// unpackage thread arguments
	struct thread_args* t_args = ((struct thread_args*)(data));
  int thread_id =(t_args->thread_id);
	bool logging = (t_args->logging);
	char* log_file = t_args->log_file;
	int* num_assignments = (t_args->num_assignments);
	int* num_pending_logs = (t_args->num_pending_logs);
	size_t* total_requests = (t_args->total_requests);
	size_t* total_errors = (t_args->total_errors);
	pthread_mutex_t *assignment_mutex = (t_args->assignment_mutex);
	pthread_mutex_t *log_mutex = (t_args->log_mutex);
	pthread_mutex_t *server_health_mutex = (t_args->server_health_mutex);
	pthread_cond_t *got_log = (t_args->got_log);
	//pthread_cond_t *got_assignment = (t_args->got_assignment);

  printf("Starting thread %d\n", thread_id);

	while (1) {
		rc = pthread_mutex_lock(assignment_mutex);
		if (rc) {
			printf(" [thread %d] error locking assignment mutex!\n", thread_id);
		}
		rc = 0;
		if (*num_assignments == 0) {
			printf(" [thread %d] waiting...\n", thread_id);
			rc = pthread_cond_wait(t_args->got_assignment, t_args->assignment_mutex);
		}

		if (rc) {
			printf(" issue with pthread_cond_wait..\n");
		}

		// got the lock!
		if (rc == 0) {
			if ((*num_assignments) > 0) {
				an_assignment = get_assignment(num_assignments);
				rc = pthread_mutex_unlock(assignment_mutex);

				if (rc) {
					printf(" [thread %d] error unlocking assignment mutex!\n", thread_id);
				}

				if (an_assignment) {

	        // handle the assignment
					printf(" [thread %d]: found an assignment!\n", thread_id);
					struct log_request* a_log_request;
	        a_log_request = perform_work(an_assignment, thread_id, log_file, total_requests, total_errors, logging, server_health_mutex);

					if (a_log_request && logging) {

						// update the health of the server
						rc = pthread_mutex_lock(server_health_mutex);
						if (isError(a_log_request->status_code)) {
							(*total_requests)++;
							(*total_errors)++;
						} else {
							(*total_requests)++;
						}
						rc = pthread_mutex_unlock(server_health_mutex);

						// pend log
						pend_log(a_log_request, log_mutex, got_log, num_pending_logs);
					}
	      	free(an_assignment);
				} else {
					printf("Error retreiving asgn!\n");
				}
			}
		}
	} // end of while
}

void* logger_thread(void* data) {
	int rc = 0;                         // return code of the pthreads functions
	int ret = 0;
	bool healthcheck = 0;

	// unpackage thread arguments
	struct logger_args* l_args = ((struct logger_args*)(data));
	char* log_file = l_args->log_file;
	pthread_mutex_t *log_mutex = (l_args->log_mutex);
	pthread_cond_t *got_log = (l_args->got_log);
	int* num_pending_logs = l_args->num_pending_logs;
	int log_fd = l_args->log_fd;



	printf("Starting logger thread...\n");

	while (1) {
		healthcheck = 0;

		rc = pthread_mutex_lock(log_mutex);
		if (rc) {
			printf(" [LOGGER] error locking log mutex!\n");
		}
		rc = 0;

		if ((*num_pending_logs )== 0) {
			printf(" [LOGGER] waiting...\n");
			rc = pthread_cond_wait(got_log, log_mutex);
		}

		// got the lock!
		if (rc == 0) {
			if ((*num_pending_logs) > 0) {
				struct log_request* a_log_request = NULL;
				printf("pending logs before get: %d", *num_pending_logs);
				a_log_request = get_pending_log(num_pending_logs);
				printf("pending logs after get: %d", *num_pending_logs);

				// after grabbing the log we're done with the lock
				rc = pthread_mutex_unlock(log_mutex);
				if (rc) {
					printf(" [LOGGER] error unlocking log mutex!\n");
				}

				// perform work
				if (a_log_request) {
					int read_fd = -1;
					char entry[100] = "";
					if(strcmp(a_log_request->res_name, "healthcheck") == 0) {
						healthcheck = 1;
					}

					// we'll need to know the size of the file
					struct stat st;
					if (stat(log_file, &st) == -1) {
							printf(" [LOGGER] Error stat'ing file (1)! log writing aborted...\n");
							free(a_log_request);
							continue;
					}

					// deal with a failure first
					switch (a_log_request->status_code) {
						case 400:
						case 403:
						case 404:
						case 500:
							strcat(entry, "FAIL: ");
							strcat(entry, a_log_request->first_line);
							strcat(entry, " --- response ");
							switch (a_log_request->status_code) {
								case 400: strcat(entry, "400\n========\n"); break;
								case 403: strcat(entry, "403\n========\n"); break;
								case 404: strcat(entry, "404\n========\n"); break;
								case 500: strcat(entry, "500\n========\n"); break;
							}

							ret = pwrite(log_fd, entry, strlen(entry), st.st_size);
							if (ret == -1) {
								printf(" [LOGGER] Error writing to log! log may be lost of corrpted...");
							}
							free(a_log_request);
							continue;
					}

					// write command
					switch (a_log_request->command) {
						case PUT: strcat(entry, "PUT /"); break;
						case HEAD: strcat(entry, "HEAD /"); break;
						case GET: strcat(entry, "GET /"); break;
					}

					// write resource name
					strcat(entry, a_log_request->res_name);

					// write length x and then newline
					char length[10] = "";
					sprintf(length, "%ld", a_log_request->content_length);
					strcat(entry, " length ");
					strcat(entry, length);
					strcat(entry,"\n");

					// if its HEAD we can add the ========\n and finish (free struct)
					if (a_log_request->command == HEAD) {
						strcat(entry, "========\n");
						ret = pwrite(log_fd, entry, strlen(entry), st.st_size);
						free(a_log_request);
						continue;
					}

					// GET/PUT: determine how many lines of data we will need
					int total_lines = a_log_request->content_length / 20;
					if (a_log_request->content_length % 20 > 0) {
						total_lines += 1;
					}

					char* log_res_name = a_log_request->res_name;
					if (!healthcheck) {
						read_fd = open(log_res_name, O_RDONLY);
					}

					// write the res_b
					ssize_t read_status = 0;
					int write_status = 0;
					uint8_t log_buffer[20];
					memset(log_buffer, 0, 20);
					size_t bytes_remaining = a_log_request->content_length;
					int offset = st.st_size;

					ret = pwrite(log_fd, entry, strlen(entry), offset);
					offset += ret;

					for (int i = 0; i < total_lines; i++) {
						char line[69] = "";

						// determine how many bytes will be printed on that line
						sprintf(line, "%.8u", write_status);

						if ((int)bytes_remaining - 20 >= 0) {
							if (healthcheck) {
								strcpy((char*)log_buffer, a_log_request->health_check_content);
							} else {
								read_status = read(read_fd, log_buffer, 20);
							}

							while((size_t)read_status != 20 && !healthcheck) {
								read_status = read(read_fd, log_buffer, 20);
							}
						} else {
							if (healthcheck) {
								strcpy((char*)log_buffer, a_log_request->health_check_content);
							} else {
								read_status = read(read_fd, log_buffer, bytes_remaining % 20);
							}

							while((size_t)read_status != bytes_remaining % 20 && !healthcheck) {
									read_status = read(read_fd, log_buffer, bytes_remaining % 20);
							}
						}

						write_status += read_status;
						if (!healthcheck) {
							bytes_remaining -= read_status;
						} else {
							bytes_remaining -= a_log_request->content_length;
							read_status = a_log_request->content_length;
						}

						for (int j = 0; j < read_status; j++) {
							sprintf((line + 8 + (3 * j)), " %.2x", log_buffer[j]);
							if (j == read_status - 1) {
								sprintf(line + ( 11 + (3 * j)), "\n");
							}
						}
						ret = pwrite(log_fd, line, 9 + (read_status * 3), offset);
						offset += ret;
					}

					char last_line[9] = "========\n";
					ret = pwrite(log_fd, last_line, 9, offset);

					free(a_log_request);
					continue;

	      }
			}
		}
	} // end of while
}

//------------------------------------------------------------------------------
//   HELPER FUNCTIONS FOR MULTITHREADING
//------------------------------------------------------------------------------

/*
  Performs the work needed for the assignment.
*/
struct log_request* perform_work(struct assignment* an_assignment, int thread_id, char* log_file, size_t* total_requests, size_t* total_errors, bool logging, pthread_mutex_t* server_health_mutex) {

	printf(" [thread %d]: performing work!\n", thread_id);
  enum command_t current_command;
  ssize_t recv_status = 0;
  char res_name[29] = "";
	char first_line[100] = "";
  ssize_t content_length = -1;
  size_t header_size = 0;
  size_t bytes_waiting = 0;
	int rc = 0;

  uint8_t buffer[BUFFER_SIZE + 1];
  memset(buffer, 0, BUFFER_SIZE+1);

	// initialize the log request and it's values
	struct log_request* a_log_request;
	a_log_request = (struct log_request*)malloc(sizeof(struct log_request));
	a_log_request->command = PUT;
	a_log_request->res_name = "";
	a_log_request->content_length = 0;
	a_log_request->status_code = 500;
	a_log_request->first_line = "";
	a_log_request->health_check_content = "";

  int client_sockd =  an_assignment->assignment_fd;
  if (client_sockd == -1) {
    printf(" [thread %d]: no client! done working...\n", thread_id);
  }

  recv_status = recv_full(client_sockd, buffer, BUFFER_SIZE);
  if (recv_status == -1) {
    printf("  [+] problem with initial recv!\n\n");
    close(client_sockd);
    return a_log_request;
  }
  buffer[recv_status] = '\0';
  printf("  [+] message recieved (%ld bytes)... \n", recv_status);

  // copy the message from the buffer to a string to use strtok_r() on:
  char request[recv_status + 1];
  strcpy(request, (char*)buffer);

	// package the first line for the logger
	if (strstr(request, "\r\n") != NULL) {
		int pos = (strstr(request,"\r\n")) - request;
		strncpy(first_line, request, pos);
		a_log_request->first_line = strndup(first_line, strlen(first_line) + 1);
	}

  // figure out how many bytes the header was by setting a null byte after
  // double CRLF. Set the remaining bytes in buffer for use with PUT if
  // necessary
  if (strstr(request,DCRLF) == NULL) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }

  *(strstr(request,DCRLF) + 4) = '\0';
  header_size = strlen(request);
  printf("    - the header size is %ld bytes\n", header_size);

  bytes_waiting = (size_t) recv_status - header_size;
  printf("    - leaving %ld bytes waiting in the buffer\n",
    bytes_waiting);

  // Begin interpreting the message
  char * token;
  char * rest = NULL;
  token = strtok_r(request, " \r\n", &rest);
  printf("\ttoken = %s\n", token);
  if (token == NULL) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }

  // method
  if (strcmp(token, "PUT") == 0) {
    a_log_request->command = current_command = PUT;
  } else if (strcmp(token, "GET") == 0) {
    a_log_request->command = current_command = GET;
  } else if (strcmp(token, "HEAD") == 0) {
    a_log_request->command = current_command = HEAD;
  } else {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }
  token = strtok_r(NULL, " \r\n", &rest);
  printf("\ttoken = %s\n", token);
  if (token == NULL) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }

  // resource name
  if (!(validate_resource(token))) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }
  strcpy(res_name, token + 1);
	a_log_request->res_name = strndup(res_name, strlen(res_name) + 1);

	// check for PUT /logfile
	if( current_command == PUT && (strcmp(res_name, log_file) == 0)) {
		a_log_request->status_code = handle_error(client_sockd, 403);
		return a_log_request;
	}

	// perform healthcheck
	if (strcmp(res_name, "healthcheck") == 0) {
		if ((current_command == PUT || current_command == HEAD)) {
			a_log_request->status_code = handle_error(client_sockd, 403);
			return a_log_request;
		} else {

			if (!logging) {
				a_log_request->status_code = handle_error(client_sockd, 404);
				return a_log_request;
			}

			char buff[100] = "";
			strcat(buff, "HTTP/1.1 200 OK\r\nContent-Length: ");

			char content_buffer[20] = "";
			char length_buffer[20] = "";

			rc = pthread_mutex_lock(server_health_mutex);
			if (rc) {
				printf(" [thread %d] error locking assignment mutex!\n", thread_id);
			}
			sprintf(content_buffer, "%ld\n%ld", *total_errors, *total_requests);
			rc = pthread_mutex_unlock(server_health_mutex);


			size_t length = strlen(content_buffer);
			sprintf(length_buffer, "%ld", length);
			strcat(buff, length_buffer);
			strcat(buff, DCRLF);
			strcat(buff, content_buffer);

			// send and clone
			send(client_sockd, buff, strlen(buff), 0);
			close(client_sockd);

			a_log_request->health_check_content = strndup(content_buffer, length + 1);
			a_log_request->content_length = length;
			a_log_request->status_code = 200;
			return a_log_request;
		}
	}
  token = strtok_r(NULL, " \r\n", &rest);
  printf("\ttoken = %s\n", token);
  if (token == NULL) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }

  // http/1.1
  if (strcmp(token, HTTP) != 0) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }
  token = strtok_r(NULL, " \r\n", &rest);
  printf("\ttoken = %s\n", token);

  // content-length header
  while (token != NULL) {
    if (strcmp(token, CONTENT_LENGTH) == 0) {
      token = strtok_r(NULL, " \r\n", &rest);
      if (token == NULL) { break; }

      char *end;
      content_length = strtol(token, &end, 10);
      if (end == token) {
        content_length = -1;
      }
    }
    token = strtok_r(NULL, " \r\n", &rest);
  }

  // if PUT make sure theres a content-length
  if (content_length == -1 && current_command == PUT) {
    a_log_request->status_code = handle_error(client_sockd, 400);
    return a_log_request;
  }
	if (current_command == GET || current_command == HEAD) {
		content_length = 0;
	}
	a_log_request->content_length = (size_t)content_length;


  // execute the request and handle the codes that are returned
  printf("  [+] EXECUTION READY:\n");
	struct stat st;
  switch (current_command) {
    case PUT:
      a_log_request->status_code = handle_error(client_sockd,
        put_executor(client_sockd, res_name, buffer, (size_t)content_length,
          bytes_waiting, header_size));
      break;
    case GET:
      a_log_request->status_code = handle_error(client_sockd,
        get_executor(client_sockd, res_name, buffer));
			if ( !(stat(res_name, &st) == -1)) {
				a_log_request->content_length = (size_t)st.st_size;
			}
      break;
    case HEAD:
      a_log_request->status_code = handle_error(client_sockd,
        head_executor(client_sockd, res_name));
				if ( !(stat(res_name, &st) == -1)) {
					a_log_request->content_length = (size_t)st.st_size;
				}
      break;
    default:
      a_log_request->status_code = handle_error(client_sockd, 500);
      break;
  }

	close(client_sockd);

	return a_log_request;
}

/*
	add_assignment()
	Adds an assignment to the list of pending assignments
*/
void add_assignment(int client_fd, pthread_mutex_t* p_mutex, pthread_cond_t* p_cond_var, int* num_assignments) {
	int rc;
	struct assignment* an_assignment;

	// create assignment
	an_assignment = (struct assignment*)malloc(sizeof(struct assignment));

	if (!an_assignment) {
		printf("[+] issue creating assignment!");
	}

	an_assignment->assignment_fd = client_fd;
	an_assignment->next = NULL;

	// lock mutex over the list
	rc = pthread_mutex_lock(p_mutex);
	if (rc) {
		printf(" [dispatcher] error locking assignment mutex!\n");
	}

	// add the request to the list
	if ((*num_assignments) == 0) {
		assignments = an_assignment;
		last_assignment = an_assignment;
	} else {
		last_assignment->next = an_assignment;
		last_assignment = an_assignment;
	}
	(*num_assignments)++;

	// unlock the mutex
	rc = pthread_mutex_unlock(p_mutex);
	if (rc) {
		printf(" [dispatcher] error unlocking assignment mutex!\n");
	}

	// signal the condition variable
	rc = pthread_cond_signal(p_cond_var);
}

/*
	 get_assignment()
	 Gets and returns an assignment from the list of pending assignments.
*/
struct assignment* get_assignment(int* num_assignments) {
	struct assignment* an_assignment;

	if ((*num_assignments) > 0) {
		an_assignment = assignments;
		assignments = assignments->next;
		if (assignments == NULL) {
				last_assignment = NULL;
		}
		(*num_assignments)--;
	}  else {
		an_assignment = NULL;
	}
	return an_assignment;
}


/*
	pend_log()
	Requests access to the pending logs list, and adds a log when granted.
	Signals logger if sleeping.
	PRE-CONDITION: a_log_request is not NULL
*/
void pend_log(struct log_request* a_log_request, pthread_mutex_t* p_mutex, pthread_cond_t* p_cond_var, int* num_pending_logs){
	int rc;

	rc = pthread_mutex_lock(p_mutex);
	if (rc) {
		printf(" error locking log mutex!\n");
	}

	if ((*num_pending_logs) == 0) {
		log_requests = a_log_request;
		last_log_request = a_log_request;
	} else {
		last_log_request->next = a_log_request;
		last_log_request = a_log_request;
	}
	(*num_pending_logs)++;

	rc = pthread_mutex_unlock(p_mutex);
	if (rc) {
		printf(" error unlocking log mutex!\n");
	}

	rc = pthread_cond_signal(p_cond_var);
}

/*
	get_pending_log()
	Extracts next log from the pending logs list.
	PRE-CONDITION: log_mutex is locked
*/
struct log_request* get_pending_log(int* num_pending_logs) {

	struct log_request* a_log_request = NULL;
	if ((*num_pending_logs) > 0) {
		a_log_request = log_requests;
		log_requests = log_requests->next;
		if (log_requests == NULL) {
				last_log_request = NULL;
		}
		(*num_pending_logs)--;
	}  else {
		a_log_request = NULL;
	}
	return a_log_request;
}
