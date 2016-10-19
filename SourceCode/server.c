/* CSci4061 S2016 Assignment 4
 * Group ID: AR
 * Name: Sujita Shah, Chaoran Chen, Cheng Chen, Arafa Alam
 * X500: shahx220, chen4393, chen4162, Alamx029 */

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "util.h"
#include <sys/timeb.h>
#include <sys/time.h>


#define MAX_THREADS 100
#define MAX_QUEUE_SIZE 100
#define MAX_REQUEST_LENGTH 64
#define MAX_CACHE_SIZE 100

FILE *log_fp;
//Structure for queue.

typedef struct request_queue
{
        int             m_socket;//fd
        char    m_szRequest[MAX_REQUEST_LENGTH];//filename
} request_queue_t;

request_queue_t *Queue;//for malloc in main()
int Queue_size = 0;// actual queue size.
int max_queue_size = 0;

//Structure for workers.
typedef struct workertype
{
	int thread_id;
	char cwd[MAX_REQUEST_LENGTH];
	int num_requests;
} worker_t;

typedef struct cache_type
{
	int fd;
	char filename[MAX_REQUEST_LENGTH];
	char *content_type;
	char *buf;
	int numbytes;
} cache_t; 

int max_cache_size = 0;//defined by user
int Cache_size = 0;//current size
cache_t *Cache;

pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;//for calling accept_connection()
pthread_mutex_t store_req_lock = PTHREAD_MUTEX_INITIALIZER;//for calling get_request()
pthread_mutex_t dsp_lock = PTHREAD_MUTEX_INITIALIZER;//for incrementing or decrementing num_dispatchers
pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;//for queue operations
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;//for log file operations
pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;//for buffer to store when fread() in worker thread
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;//for ache operations
pthread_cond_t not_empty;//so that we can dequeue
pthread_cond_t not_full;//so that we can enqueue
int num_dispatchers;

void iLock(pthread_mutex_t *mutex) {
	int error;
	if(error = pthread_mutex_lock(mutex))
		fprintf(stderr, "Failed to lock!\n", strerror(error));
}

void iUnlock(pthread_mutex_t *mutex) {
	int error;
	if(error = pthread_mutex_unlock(mutex))
		fprintf(stderr, "Failed to unlock!\n", strerror(error));
}

void enqueue(request_queue_t *queue, request_queue_t item) {
	if(Queue_size == max_queue_size)
		printf("Failed to enqueue!\n");
	Queue_size++;
	queue[Queue_size - 1] = item;
}

request_queue_t dequeue(request_queue_t *queue) {
    if(Queue_size == 0)
        printf("Failed to dequeue!\n");
    request_queue_t empty;//for clearing the slot
    request_queue_t target;//the request to pick up
    int q_idx;
    target = Queue[0];//pick up the first element in queue
    for (q_idx = 0; q_idx < Queue_size; q_idx++) {
        if (q_idx == Queue_size - 1)
            queue[q_idx] = empty;
        else
            queue[q_idx] = queue[q_idx + 1];
    }
    Queue_size--;
    return target;
}

cache_t decache(cache_t *cache) {
    if(Cache_size == 0)
        printf("Failed to decache!\n");
    cache_t empty;//for clearing the slot
    cache_t target;//the information to pick up
    int c_idx;
    target = Cache[0];//pick up the first element in cache
    for (c_idx = 0; c_idx < Cache_size; c_idx++) {
        if (c_idx == Cache_size - 1)
            cache[c_idx] = empty;
        else
            cache[c_idx] = cache[c_idx + 1];
    }
    Cache_size--;
    return target;
}

void encache(cache_t *cache, int fd, char *filename, char* content_type, char *buf, int bytes_read, int size) {
	
	if(Cache_size == max_cache_size) {
		decache(cache);
	}
	cache[Cache_size].fd = fd;
	memcpy(cache[Cache_size].filename, filename, strlen(filename) + 1);
	cache[Cache_size].content_type = (char *)malloc(12 * sizeof (char));
	strcpy(cache[Cache_size].content_type, content_type);
	cache[Cache_size].buf =  buf;
	cache[Cache_size].numbytes = bytes_read;
	Cache_size++;
}

void * dispatch(void * arg)
{
	while(1) {
		request_queue_t request;
		request.m_socket = accept_connection();
		
		//if the return value of accept_connection() is negative, ignore it
		if(request.m_socket < 0) {
			fprintf(stderr, "Failed to call accept_connection()!\n");
		}
		

		int retv_get_req = get_request(request.m_socket, request.m_szRequest);

		if(retv_get_req == 0) {
			iLock(&q_lock);
			while(Queue_size == max_queue_size)
				pthread_cond_wait(&not_full, &q_lock);
			enqueue(Queue, request);
			pthread_cond_signal(&not_empty);
			iUnlock(&q_lock);
		}
		else
			printf("Failed to get request.\n");
	}
	
	iLock(&dsp_lock);
	num_dispatchers--;
	iUnlock(&dsp_lock);
	return NULL;
}

void * worker(void * arg)
{
	
	worker_t * worker_struct = NULL;
	worker_struct = (worker_t *) malloc(sizeof (worker_t));
	
	if(worker_struct == NULL) {
		printf("Failed to obtain worker thread info!\n");
		return NULL;
	}
	memcpy(worker_struct, (worker_t *) arg, sizeof (worker_t));
	
	while(1) {
		
		iLock(&dsp_lock);
		if(num_dispatchers == 0 && Queue_size == 0){
			free(worker_struct);
			pthread_exit(NULL);
		}
		iUnlock(&dsp_lock);
		
		request_queue_t new_request;
		iLock(&q_lock);
		
		while(Queue_size == 0)
			pthread_cond_wait(&not_empty, &q_lock);
		
		new_request = dequeue(Queue);
		
		worker_struct->num_requests++;//increment the accumulative number of requests once we handle a request
		
		pthread_cond_signal(&not_full);
		
		iUnlock(&q_lock);
		
		
		struct timeval tmb1, tmb2, tmb3;
		gettimeofday(&tmb1,NULL);
		
// -------------------------Cache part------------------------------------------------------------------
		
		//traverse cache to find that request
		int cache_idx;
		int cache_hit_flag = 0;
		//int time_interval = 0;
		for(cache_idx = 0; cache_idx < max_cache_size; cache_idx++) {
			if(!strcmp(Cache[cache_idx].filename, new_request.m_szRequest)) { //Cache[cache_idx].fd == new_request.m_socket
				//Cache hit!
				cache_hit_flag = 1;
				break;
			}
		}
//-------------------------------Cache hit--------------------------------------------------------------------------		
		if (cache_hit_flag == 1) {

			if(return_result(new_request.m_socket, Cache[cache_idx].content_type, Cache[cache_idx].buf, Cache[cache_idx].numbytes) != 0)
				fprintf(stderr, "Failed to call return_result()!\n");
			
			
			
			//MODIFIED, time interval calculation
		    int difference;
		    gettimeofday(&tmb2,NULL);
		    difference = (int)((tmb2.tv_sec-tmb1.tv_sec)*1000000 + (tmb2.tv_usec- tmb1.tv_usec));
		    iLock(&log_lock);
			fprintf(log_fp, "[%d][%d][%d][%s][%d][%dus][%s]\n", worker_struct->thread_id, worker_struct->num_requests, new_request.m_socket, new_request.m_szRequest, Cache[cache_idx].numbytes, difference, "HIT");
			fflush(log_fp);
			iUnlock(&log_lock);
			cache_hit_flag = 0;
		}
		
// -------------------------Disk part-------------------------------------------------------------------	
		else {
			//Read the file from fd
			size_t bytes_read = 0;
			
			char path[MAX_REQUEST_LENGTH]; 
			strcpy(path, worker_struct->cwd);
			strcat(path, new_request.m_szRequest);
			
			FILE * new_fp = fopen(path, "rb");
			
			int size;
			// Invalid file
			if(new_fp == NULL) {
				printf("Failed to open the file!\n");
				
				printf("Return error!\n");
				return_error(new_request.m_socket, "File not found.");
				//lock
				iLock(&log_lock);
				fprintf(log_fp, "[%d][%d][%d][%s][%s]\n", worker_struct->thread_id, worker_struct->num_requests, new_request.m_socket, new_request.m_szRequest, "File not found.");
				fflush(log_fp);
				iUnlock(&log_lock);
				continue;
			}
			else {
				int new_fd = fileno(new_fp);
				struct stat statStruct;
				if(fstat(new_fd, &statStruct) >= 0)
					size = (int) statStruct.st_size;//obtain the actual size of the file
				// Invalid file size
				else {
					printf("Failed to retrieve size from fstat!\n");
					printf("Return error!\n");
					return_error(new_request.m_socket, "File not found.");
					//lock
					iLock(&log_lock);
					fprintf(log_fp, "[%d][%d][%d][%s][%s]\n", worker_struct->thread_id, worker_struct->num_requests, new_request.m_socket, new_request.m_szRequest, "File not found.");
					fflush(log_fp);
					iUnlock(&log_lock);
					continue;
				}
			}
			iLock(&buf_lock);

			char *read_buf = (char *) malloc(sizeof(char)*(size + 1));
			iUnlock(&buf_lock);
			
			bytes_read = fread(read_buf, sizeof(char), size, new_fp);//disk operation
			// Read size in compatible with file size
			if(bytes_read != size) {
				fputs("Failed to read!\n", stderr);
				printf("Return error!\n");
					return_error(new_request.m_socket, "File not found");
					//lock
					iLock(&log_lock);
					fprintf(log_fp, "[%d][%d][%d][%s][%s]\n", worker_struct->thread_id, worker_struct->num_requests, new_request.m_socket, new_request.m_szRequest, "File not found");
					fflush(log_fp);
					iUnlock(&log_lock);
					continue;
			}
			
			//Prepare to serve the request or return the error
			char type[12];
			// Valid read
			if(bytes_read >= 0) {
				fclose(new_fp);
				if(strstr(new_request.m_szRequest, ".html") || strstr(new_request.m_szRequest, ".htm"))
					strcpy(type, "text/html");
				if(strstr(new_request.m_szRequest, ".jpg"))
					strcpy(type, "image/jpeg");
				if(strstr(new_request.m_szRequest, ".gif"))
					strcpy(type, "image/gif");
				else
					strcpy(type, "text/plain");

				if(return_result(new_request.m_socket, type, read_buf, bytes_read) != 0)
					fprintf(stderr, "Failed to call return_result()!\n");
			}
			
//-------------------------------------save in cache----------------------------------------------------------------lock
			iLock(&cache_lock);
			encache(Cache, new_request.m_socket, new_request.m_szRequest, type, read_buf, bytes_read, size);
			iUnlock(&cache_lock);
//-----------------------------------------------------------------------------------------------------unlock
			
			
//--------------------------------------Cache miss--------------------------------------------------------------------------
            
			//MODIFIED, time interval calculation
			//usleep(1000);
			gettimeofday(&tmb3,NULL);
			int difference;

		    difference = (int) ((tmb3.tv_sec-tmb1.tv_sec)*1000000 + (tmb3.tv_usec- tmb1.tv_usec));
		    
		    //log operation for disk operation.
			iLock(&log_lock);
			fprintf(log_fp, "[%d][%d][%d][%s][%d][%dus][%s]\n", worker_struct->thread_id, worker_struct->num_requests, new_request.m_socket, new_request.m_szRequest, bytes_read, difference, "MISS");
			fflush(log_fp);
			iUnlock(&log_lock);
		}	
		
	}
	free(worker_struct);
    return NULL;
}

int main(int argc, char **argv)
{
        // Error check first.
        if(argc != 6 && argc != 7)
        {
                printf("usage: %s port path num_dispatcher num_workers queue_length [cache_size]\n", argv[0]);
                return -1;
        }
        if(atoi(argv[1]) < 1025 || atoi(argv[1]) > 65535)
        {
			fprintf(stderr, "Invalid port number! Please input a port number between 1025 and 65535.\n");
			return -1;
		}
        if(chdir(argv[2]) == -1) 
        {
			printf("Failed to change current working directory to %s\n", argv[2]);
			return -1;
		}
		if(atoi(argv[3]) > MAX_THREADS)
		{
			fprintf(stderr, "Invalid number! It exceeded the maximum number of dispatcher threads.\n");
			return -1;
		}
		if(atoi(argv[4]) > MAX_THREADS)
		{
			fprintf(stderr, "Invalid number! It exceeded the maximum number of worker threads.\n");
			return -1;
		}
		if(atoi(argv[5]) > MAX_QUEUE_SIZE)
		{
			fprintf(stderr, "Invalid number! It exceeded the maximum queue size.\n");
			return -1;
		}
		if(atoi(argv[6]) > MAX_QUEUE_SIZE)
		{
			fprintf(stderr, "Invalid number! It exceeded the maximum cache size.\n");
			return -1;
		}
		
		// malloc queue
		max_queue_size = atoi(argv[5]);
		if(max_queue_size <= MAX_QUEUE_SIZE)
			Queue = (request_queue_t *)malloc(max_queue_size * sizeof(request_queue_t));
		//malloc cache
		max_cache_size = atoi(argv[6]);
		if(max_cache_size <= MAX_CACHE_SIZE)
			Cache = (cache_t *)malloc(max_cache_size * sizeof(cache_t));
		
		init(atoi(argv[1]));
		
		//Generate log file
		log_fp = fopen("web_server_log", "w+");
		
		int i;
		int error;
		pthread_t dispatchers[MAX_THREADS], workers[MAX_THREADS];
		//Initialize CVs
		int error_cv;
		if(error_cv = pthread_cond_init(&not_empty, NULL))
			fprintf(stderr, "Failed to initialize get_req_cond:%s\n", strerror(error_cv));
		if(error_cv = pthread_cond_init(&not_full, NULL))
			fprintf(stderr, "Failed to initialize get_req_cond:%s\n", strerror(error_cv));
		//Create dispatchers and workers
		for(i = 0; i < atoi(argv[3]); i++) {
			if(error = pthread_create(&dispatchers[i], NULL, dispatch, NULL)) {
				fprintf(stderr, "Failed to create the dispatcher thread %d: %s\n", i + 1, strerror(error));
				return -1;//Should we return?
			}
		}
		num_dispatchers = atoi(argv[3]);
		for(i = 0; i < atoi(argv[4]); i++) {
			worker_t worker_struct;
			worker_struct.thread_id = i;
			strcpy(worker_struct.cwd, argv[2]);
			worker_struct.num_requests = 0;
			if(error = pthread_create(&workers[i], NULL, worker, &worker_struct)) {
				fprintf(stderr, "Failed to create the worker thread %d: %s\n", i + 1, strerror(error));
				return -1;//Should we return?
			}
			usleep(100);
			
		}
        printf("Call init() first and make a dispather and worker threads\n");
        for(i = 0; i < atoi(argv[4]); i++) {
			if(error = pthread_join(workers[i], NULL))
				fprintf(stderr, "Failed to join the worker thread %d: %s\n", i + 1, strerror(error));
		}
		for(i = 0; i < atoi(argv[3]); i++) {
			if(error = pthread_join(dispatchers[i], NULL))
				fprintf(stderr, "Failed to join the dispatcher thread %d: %s\n", i + 1, strerror(error));
		}
		if(fclose(log_fp))
			fprintf(stderr, "Failed to close the log file!\n");
		if(pthread_mutex_destroy(&req_lock))
			fprintf(stderr, "Failed to destroy req_lock!\n");
		if(pthread_mutex_destroy(&store_req_lock))
			fprintf(stderr, "Failed to destroy store_req_lock!\n");
		if(pthread_mutex_destroy(&dsp_lock))
			fprintf(stderr, "Failed to destroy dsp_lock!\n");
		if(pthread_mutex_destroy(&q_lock))
			fprintf(stderr, "Failed to destroy q_lock!\n");
		if(pthread_mutex_destroy(&log_lock))
			fprintf(stderr, "Failed to destroy log_lock!\n");
		if(pthread_mutex_destroy(&buf_lock))
			fprintf(stderr, "Failed to destroy buf_lock!\n");
		if(pthread_cond_destroy(&not_empty))
			fprintf(stderr, "Failed to destroy not_empty!\n");
		if(pthread_cond_destroy(&not_full))
			fprintf(stderr, "Failed to destroy not_full!\n");

		free(Queue);
		for (i = 0; i < max_cache_size; i++) {
			free(Cache[i].content_type);
			free(Cache[i].buf);
		}
		free(Cache);
        return 0;
}
