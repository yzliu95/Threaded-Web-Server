/*
 * CSCI4061 Fall 2018 Project 3
 * Name: Yuanzhe Liu
 * X500: yuanz002
*/
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "util.h"

#define SUCCESS 1
#define FAILURE 0
#define MAX_THREADS 100
#define MAX_QUEUE_LEN 100
#define MAX_CE 100
#define PATH_SIZE 1024
#define OLDER 0
#define NEWER 1
#define INVALID -1

// structs:
typedef struct request_entry {
    int fd;
    char *request;
} request_t;

typedef struct cache_entry {
    int len;
    char *request;
    char *content;
} cache_entry_t;

//arguments
int port, num_dispatcher, num_workers, dynamic_flag, queue_length, cache_size;
char path[PATH_SIZE];

// queue global variables
request_t *queue;
int queue_out = 0;
int queue_in = -1;
int req_in_que = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_slot_cv = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_empty_cv = PTHREAD_COND_INITIALIZER;

// cache global variables
int** cache_list;
int newest_end;
int oldest_end;
int cache_count= 0;
cache_entry_t *cache;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// thread global variables
pthread_mutex_t thread_counter_lock = PTHREAD_MUTEX_INITIALIZER;
int worker_id_counter = 0;

// logging variables
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
FILE* log_file;

/* *************************************************************************/
//  cache functions
/* *************************************************************************/

// Function to check whether the given request is present in cache
int getCacheIndex(char *request) {
  pthread_mutex_lock(&cache_lock);
    int i = 0;
    for (i = 0; i < cache_size; i++) {
        if (cache[i].len > 0) {
            if (strcmp(cache[i].request, request) == 0) {
              int old = cache_list[i][OLDER];
              int new = cache_list[i][NEWER];
              cache_list[old][NEWER] = new;
              cache_list[new][OLDER] = old;


              int temp = cache_list[newest_end][OLDER];
              cache_list[temp][NEWER] = i;
              cache_list[newest_end][OLDER] = i;
              cache_list[i][OLDER] = temp;
              cache_list[i][NEWER] = newest_end;
              pthread_mutex_unlock(&cache_lock);
              return i;
            }
          }
    }
    pthread_mutex_unlock(&cache_lock);
    return -1;
}

// Function to add the request and its file content into the cache
void addIntoCache(char *request, char *content, int content_length) {
  pthread_mutex_lock(&cache_lock);

  int idx = -1;

  if (cache_count >= cache_size) {
    idx = cache_list[oldest_end][NEWER];
    int temp = cache_list[idx][NEWER];
    cache_list[oldest_end][NEWER] = temp;
    cache_list[temp][OLDER] = oldest_end;
    free(cache[idx].request);
    free(cache[idx].content);
    cache[idx].len = 0;
  } else {
    idx = cache_count;
    cache_count ++;
  }

  cache[idx].request = malloc(PATH_SIZE);
  cache[idx].content = malloc(content_length);
  strcpy(cache[idx].request, request);
  memcpy(cache[idx].content, content, content_length);
  cache[idx].len = content_length;

  int temp = cache_list[newest_end][OLDER];
  cache_list[temp][NEWER] = idx;
  cache_list[newest_end][OLDER] = idx;
  cache_list[idx][OLDER] = temp;
  cache_list[idx][NEWER] = newest_end;
  pthread_mutex_unlock(&cache_lock);
}

// Function to initialize the cache
void initCache() {
  cache = malloc(cache_size * sizeof(cache_entry_t));
  int i = 0;
  for (i = 0; i < cache_size; i++) {
    cache[i].len = 0;
    cache[i].request = NULL;
    cache[i].content = NULL;
  }
  cache_list = malloc((cache_size + 2)*sizeof(int*));
  for (int i = 0; i< (cache_size + 2); i++) {
    cache_list[i] = malloc(2 *sizeof(int));
  }

  for(int i = 0; i < (cache_size + 2); i++) {
    for(int j = 0; j < 2; j ++) {
      cache_list[i][j] = INVALID;
    }
  }
  oldest_end = cache_size;
  newest_end = cache_size + 1;
  cache_list[oldest_end][NEWER] = newest_end;
  cache_list[newest_end][OLDER] = oldest_end;
}

/**********************************************************************************/
// Queue functions
/**********************************************************************************/

// Function to get next request from queue - should block if nothing in queue
int acquireRequest(char *request) {
  pthread_mutex_lock(&queue_lock);
  while (req_in_que <= 0) {
    pthread_cond_wait(&queue_empty_cv, &queue_lock);
  }

  int fd = queue[queue_out].fd;
  queue[queue_out].fd = -1;
  strcpy(request, queue[queue_out].request);
  free(queue[queue_out].request);

  queue_out++;
  if (queue_out == queue_length) queue_out = 0;
  req_in_que--;
  pthread_cond_signal(&queue_slot_cv);
  pthread_mutex_unlock(&queue_lock);
  return fd;
}

// Function to initialize the queue
void initQueue() {
  queue = malloc(queue_length *sizeof(request_t));
  for (int i = 0; i < queue_length; i++) {
    queue[i].fd = -1;
    queue[i].request = NULL;
  }
}

/**********************************************************************************/
// End Queue functions
/**********************************************************************************/
// Function to open and read the file from the disk into the memory
int readFromDisk(char *request, char *content, int content_length) {
  char cwd[PATH_SIZE];
  if (getcwd(cwd, sizeof(cwd)) == NULL) fprintf(stdout, "Failed to get cwd\n");
  char abs_path[PATH_SIZE];
  strcpy (abs_path,cwd);
  strcat (abs_path,request);

  FILE *file = fopen(abs_path, "r");
  if (file == NULL) {
    fprintf(stdout, "Failed to open file\n");
    return -1;
  }
  int num_byte = fread(content, content_length, 1, file);
  if (EOF == fclose(file)) {
    fprintf(stdout, "Failed to close file\n");
    return -1;
  }
  return num_byte;
}

// Function to get the target file size, return the size of file on success, -1 otherwise
int getFileSizeOnDisk(char *request) {
  char cwd[PATH_SIZE];
  if (getcwd(cwd, sizeof(cwd)) == NULL) fprintf(stdout, "Failed to get cwd\n");
  char abs_path[PATH_SIZE];
  strcpy (abs_path,cwd);
  strcat (abs_path,request);

  FILE *fd = fopen(abs_path, "r");
  if (fd == NULL) {
    fprintf(stdout, "Failed to open file\n");
    return -1; }
  if (fseek(fd, 0, SEEK_END)) {
        fprintf(stdout, "Failed to set seek position to end\n");
        return -1; }
  int size = ftell(fd);
  if (size == -1) {
    fprintf(stdout, "Failed to get file size\n");
    return -1; }
  if (fseek(fd, 0, SEEK_SET)) {
    fprintf(stdout, "Failed to reset seek position\n");
    return -1; }
  if (fclose(fd)) {
    fprintf(stdout,"Failed to close file\n");
    return -1; }
  return size;
}

// print specific log info to both terminal and log file, thread_safe
void log_line(char *log_info) {
  fprintf(stdout, "%s", log_info);
  pthread_mutex_lock(&log_lock);
  fprintf(log_file,"%s", log_info);
  fflush(log_file);
  pthread_mutex_unlock(&log_lock);
}

/**********************************************************************************/

/* ************************************ Utilities *******************/
// Return the content type based on the file suffix
char *getContentType(char *mybuf) {
  char* ext = strrchr(mybuf, '.');
  if (!ext) {
    /* no extension */
    return "text/plain";
  } else {
    if (strcmp (ext, ".html") == 0) return "text/html";
    if (strcmp (ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp (ext, ".gif") == 0) return "image/gif";
    return ".text/plain";
  }
}

// This function returns the current time in microseconds
int getCurrentTimeInMicro() {
    struct timeval curr_time;
    gettimeofday(&curr_time, NULL);
    return curr_time.tv_sec * 1000000 + curr_time.tv_usec;
}

/**********************************************************************************/

// Function to receive the request from the client and add to the queue
void *dispatch(void *arg) {
  // polling
  while (1) {
    // Accept client connection
    int fd = accept_connection();
    if (fd < 0) {
      fprintf(stdout, "Connection error\n");
      continue;
    }

    // Get request from the client
    char request[PATH_SIZE];
    if (0 != get_request(fd, request)) {
      fprintf(stdout, "Failed to get request\n");
      continue;
    }

    // Store request into queue
    pthread_mutex_lock(&queue_lock);
    while (queue_length <= req_in_que) {  // wait for req_que to open slot
        pthread_cond_wait(&queue_slot_cv, &queue_lock);
    }
    if (req_in_que < queue_length) {
      if (queue_in == queue_length - 1) {
        queue_in = -1;
      }
      ++queue_in;
      queue[queue_in].fd = fd;
      queue[queue_in].request = malloc(PATH_SIZE);
      memcpy(queue[queue_in].request, request, PATH_SIZE);
      req_in_que++;
      pthread_cond_signal(&queue_empty_cv);
    }
  pthread_mutex_unlock(&queue_lock);
} /* while ...*/
}

/**********************************************************************************/

// Function to retrieve the request from the queue, process it and then return a
// result to the client
void *worker(void *arg) {
  pthread_mutex_lock(&thread_counter_lock);
  int tid = worker_id_counter;
  worker_id_counter ++;
  pthread_mutex_unlock(&thread_counter_lock);
  int services = 0;

  // polling
  while (1) {
    char error[60];
    int i = 0;
    char log_info[PATH_SIZE];
    services ++;
    int status = FAILURE;
    char* content;
    int content_length = 0;
    char request[PATH_SIZE];
    strcpy(error, "Error: 001");
    // Get the request from the queue
    int fd = acquireRequest(request);
    // Start recording time
    unsigned int t = getCurrentTimeInMicro();
    char *type = getContentType(request);

    // this while is used so when condition satisified, the program
    // can continue in the polling loop
    while(1){
      int index = getCacheIndex(request);
      // if MISS
      if (index == -1) {
        content_length = getFileSizeOnDisk(request);
        if (content_length == -1) {
          strcpy (error, "File unavailable");
          t = getCurrentTimeInMicro() - t;
          sprintf(log_info, "[%u][%u][%u][%s][%s][%u us][MISS]\n",
                  tid, services, fd, request, error, t);
          break;
        }
        content = malloc(content_length);
        if (readFromDisk(request, content, content_length) == -1) {
          fprintf(stdout, "Failed to read file\n");
          strcpy (error, "File reading error");
          t = getCurrentTimeInMicro() - t;
          sprintf(log_info, "[%u][%u][%u][%s][%s][%u us][MISS]\n", tid,
                  services, fd, request, error, t);
          free(content);
          break;
        }
        addIntoCache(request, content, content_length);
        t = getCurrentTimeInMicro() - t;
        sprintf(log_info, "[%u][%u][%u][%s][%u][%u us][MISS]\n", tid,
                services, fd, request, content_length, t);
        status = SUCCESS;
        break;
      } else { // cache HIT
        pthread_mutex_lock(&cache_lock);
        content = malloc(cache[index].len);
        content_length = cache[index].len;
        memcpy(content, cache[index].content, cache[index].len);
        pthread_mutex_unlock(&cache_lock);
        t = getCurrentTimeInMicro() - t;
        sprintf(log_info, "[%u][%u][%u][%s][%u][%u us][HIT]\n", tid,
                services, fd, request, content_length, t);
        status = SUCCESS;
        break;
      }
    }
    // return the result and record log
    if (status == SUCCESS) {
      return_result(fd, type, content, content_length);
      log_line(log_info);
    } else {
      return_error(fd,error);
      log_line(log_info);
    }
  }
}

/**********************************************************************************/

int main(int argc, char **argv) {
  // Error check on number of arguments
  // Decided to check if caching is enabled [argc == 8 -> Caching enabled]
  if (argc != 7 && argc != 8) {
    printf("usage: %s port path num_dispatcher num_workers dynamic_flag queue_length cache_size\n", argv[0]);
    return -1;
  }
  // Get the input args
  // Perform error checks on the input arguments
  port = atoi(argv[1]);
  strcpy(path, argv[2]);
  printf("port = %d \n", port);
  printf("path = %s \n", path);
  num_dispatcher = atoi(argv[3]);
  printf("num_dispatcher = %d \n", num_dispatcher);
  num_workers = atoi(argv[4]);
  printf("num_workers = %d \n", num_workers);
  dynamic_flag = atoi(argv[5]);
  printf("dym_flag = %d \n", dynamic_flag);
  queue_length = atoi(argv[6]);
  printf("queue_length = %d \n", queue_length);
  cache_size = atoi(argv[7]);
  printf("cache size = %d \n", cache_size);

  if (port > 65535 || port < 1025){
    printf("usage: port should be between 1025-65535, input: %d\n", port);
    return -1;
  } else if (num_dispatcher > MAX_THREADS || num_dispatcher <= 0){
    printf("usage: dispatch threads should be between 0-100, input: %d\n", num_dispatcher);
    return -1;
  } else if (num_workers > MAX_THREADS || num_workers <= 0){
    printf("usage: worker threads should be between 0-100, input: %d\n", num_workers);
    return -1;
  } else if (cache_size > MAX_CE || cache_size <= 0){
    printf("usage: cache should be between 0-100, input: %d\n", cache_size);
    return -1;
  } else if (queue_length > MAX_QUEUE_LEN || queue_length <= 0){
    printf("usage: queue length should be between 0-100, input: %d\n", queue_length);
    return -1;
  }

  // create a log file
  log_file = fopen("./web_server_log", "a");
    // Change the current working directory to server root directory
  if (-1 == chdir(path)) {
    fprintf(stdout, "issue changing to desired root server path\n");
    exit(-1);
  }

  // Start the server and initialize cache and initialize queue
  init(port);
  initCache();
  initQueue();


  // Create dispatcher and worker threads
  pthread_t* dispatch_threads = malloc(num_dispatcher * sizeof(pthread_t));
  pthread_t* worker_threads = malloc(num_workers * sizeof(pthread_t));
  for (int i = 0; i < num_dispatcher; i++) {
    pthread_create(&dispatch_threads[i], NULL, &dispatch, NULL);
  }
  for (int i = 0; i < num_workers; i++) {
    pthread_create(&worker_threads[i], NULL, &worker, NULL);
  }

  while (1);
  // No manual clean up needed since the program is blocked and can only be ended
  // by crtl-c, when main thread ends, the whole process ends and all memory will
  // be freeed
  return 0;
}
