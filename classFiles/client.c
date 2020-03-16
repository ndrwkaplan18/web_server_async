/* Generic */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Network */
#include <netdb.h>
#include <sys/socket.h>


#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>


#define BUF_SIZE 250

/* What a worker thread needs to connect to host */
typedef struct {
	char * hostname;
  char * portnum;
  int num_threads;
  int isFIFO;
  char * filename1;
  char * filename2;
} host_t;

static host_t the_host;
// define type for worker thread C function
typedef void * (worker_fn) (void *);

// Source I used on how to use barriers: http://man7.org/tlpi/code/online/dist/threads/pthread_barrier_demo.c.html
pthread_barrier_t barrier;
// Source for semaphore usage: https://sites.google.com/site/spaceofjameschen/home/multi-threading/thread-synchronization-using-semaphore-linux
sem_t * binary_sem;

pthread_mutex_t work_mutex;

/************************************************************************************************************************************/
/************************************************************************************************************************************/
/* FUNCTION DECLARATIONS */
void threads_init(size_t num_threads, worker_fn *worker);
static void *client_worker(void *arg);
void host_init(host_t *host, int argc, char * hostname, char * portnum, size_t num_threads, char * schedalg, char * filename1, char * filename2);
struct addrinfo *getHostInfo(char* host, char* port);
int establishConnection(struct addrinfo *info);
void GET(int clientfd, char *path);


/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*THREAD FUNCTIONS */
void threads_init(size_t num_threads, worker_fn *worker)
{
	pthread_t* threads = (pthread_t*) malloc(sizeof(pthread_t) * num_threads); // array of threads
  if(the_host.isFIFO)
    binary_sem = (sem_t *) malloc(sizeof(sem_t) * (num_threads + 1)); // array of semaphores, but we want it 1-indexed so threadID = semaphore index
	size_t	i;
	int status;

  for (i=0; i < num_threads; i++) {
    // If schedalg is FIFO, initialize each semaphore in the closed position
    if((status = (the_host.isFIFO) ? sem_init(&binary_sem[i + 1], 0, 0) : 0) != 0){
      perror("semaphore initialization failed");
      exit(EXIT_FAILURE);
    }
		if((status = pthread_create(&threads[i], NULL, *worker, (void *) (i + 1))) == 0){
			pthread_detach(threads[i]); // make non-joinable
		}
		else
			printf("Oops, pthread_create() returned error code %d when attempting to make thread %d\n",status, (int) i);
	}
  // Once all threads are created, wake up thread 1
  if(the_host.isFIFO) sem_post(&binary_sem[1]);
}

static void *client_worker(void *arg){
  if(the_host.isFIFO) sem_wait(&binary_sem[(intptr_t) arg]);
  int my_id = (intptr_t) arg; // Just casting to (int) triggers warning: "cast to pointer from integer of different size"
	// https://stackoverflow.com/questions/21323628/warning-cast-to-from-pointer-from-to-integer-of-different-size
  host_t *host = &the_host;
  int clientfd, s, i = 0, first = 1;
  char buf[BUF_SIZE];

  while(1){
    // Check if this is the first iteration.
    if(the_host.isFIFO && !first) sem_wait(&binary_sem[my_id]);
    else first = 0;
    // Establish connection with <hostname>:<port>
    clientfd = establishConnection(getHostInfo(host->hostname, host->portnum));

    if (clientfd == -1) {
      if(host->filename2 == NULL || i == 0){
        fprintf(stderr,
              "[client_worker:88] Failed to connect to: %s:%s%s \n",
              host->hostname, host->portnum, host->filename1);
      } else {
        fprintf(stderr,
              "[client_worker:92] Failed to connect to: %s:%s%s \n",
              host->hostname, host->portnum, host->filename2);
      }
        pthread_barrier_wait(&barrier);
        continue;
    }

    fprintf(stdout, "\nThread %d here, sending GET request now.\n",my_id);
    if(host->filename2 == NULL || i == 0)
      GET(clientfd, host->filename1);
    else
      GET(clientfd, host->filename2);
    // Wake up next thread unless this is the last one
    if(host->isFIFO && my_id != host->num_threads) sem_post(&binary_sem[my_id + 1]);

    while (recv(clientfd, buf, BUF_SIZE, 0) > 0) {
      fputs(buf, stdout);
      memset(buf, 0, BUF_SIZE);
    }

    close(clientfd);

    i = (i == 0) ? 1 : 0;

    s = pthread_barrier_wait(&barrier);
    if(s == PTHREAD_BARRIER_SERIAL_THREAD)
      printf("\n");
  }
  return NULL;
}

/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*CLIENT FUNCTIONS */
void host_init(host_t *host, int argc, char * hostname, char * portnum, size_t num_threads, char * schedalg, char * filename1, char * filename2){
  host->hostname = hostname;
  host->portnum = portnum;
  host->num_threads = num_threads;
  host->isFIFO = !strncmp(schedalg,"FIFO",4);
  // fprintf(stdout,"schedalg %s\n",schedalg);
  host->filename1 = filename1;
  if(argc == 7)
    host->filename2 = filename2;
}

// Get host information (used to establishConnection)
struct addrinfo *getHostInfo(char* host, char* port) {
  int r;
  struct addrinfo hints, *getaddrinfo_res;
  // Setup hints
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if ((r = getaddrinfo(host, port, &hints, &getaddrinfo_res))) {
    fprintf(stderr, "[getHostInfo:21:getaddrinfo] %s\n", gai_strerror(r));
    return NULL;
  }

  return getaddrinfo_res;
}

// Establish connection with host
int establishConnection(struct addrinfo *info) {
  if (info == NULL) return -1;

  int clientfd;
  for (;info != NULL; info = info->ai_next) {
    if ((clientfd = socket(info->ai_family,
                           info->ai_socktype,
                           info->ai_protocol)) < 0) {
      perror("[establishConnection:35:socket]");
      continue;
    }

    if (connect(clientfd, info->ai_addr, info->ai_addrlen) < 0) {
      close(clientfd);
      perror("[establishConnection:42:connect]");
      continue;
    }

    freeaddrinfo(info);
    return clientfd;
  }

  freeaddrinfo(info);
  return -1;
}

// Send GET request
void GET(int clientfd, char *path) {
  char req[1000] = {0};
  sprintf(req, "GET %s HTTP/1.0\r\n\r\n", path);
  send(clientfd, req, strlen(req), 0);
}

/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*MAIN */
int main(int argc, char **argv) {
  int s;
  host_t *host = &the_host;
  worker_fn *worker = client_worker;

  if (argc != 6 && argc != 7) {
    fprintf(stderr, "USAGE: %s <hostname> <port> <threads> <schedalg> <request path1> <optional request path 2>\n", argv[0]);
    return 1;
  }
  host_init(host, argc, argv[1], argv[2], atoi(argv[3]), argv[4], argv[5], argv[6]);

  if((s = pthread_barrier_init(&barrier, NULL, host->num_threads + 1)) != 0){
    perror("barrier initialization failed");
    exit(EXIT_FAILURE);
  }

  threads_init(host->num_threads, *worker);

  while(1){
    pthread_barrier_wait(&barrier);
    if(host->isFIFO) sem_post(&binary_sem[1]); // Wake up thread 1
  }

  return 0;
}
