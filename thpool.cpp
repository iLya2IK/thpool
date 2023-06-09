/* ********************************
 * Author:       Johan Hanssen Seferidis
 * License:	     MIT
 * Description:  Library providing a threading pool where you can add
 *               work. For usage, check the thpool.h file or README.md
 *
 */
/*! \note Memory leak fixed in bsem_reset (2023) Medvedkov I
 *  \note Throw exception on err added (C++) (2023) Medvedkov I
 *  \note CodeBlocks project added (2023) Medvedkov I
 *
 *  \file thpool.cpp
 *
 ********************************/

#include "thpool.h"
#include <exception>
#include <string>

#define _POSIX_C_SOURCE 200809L
#define DISABLE_PRINT

#ifdef LINUX
#include <unistd.h>
#include <time.h>
#elif WIN64
#include <synchapi.h>
#endif
#ifdef LINUX
#include <sched.h>
#include <cpuid.h>
#define DO_SLEEP0ms nanosleep((const struct timespec[]){{0, 100L}}, NULL)
#define DO_SLEEP1ms nanosleep((const struct timespec[]){{0, 1000000L}}, NULL)
#else
#define DO_SLEEP0ms Sleep(0)
#define DO_SLEEP1ms Sleep(1)
#endif

#ifdef THPOOL_DEBUG
#define THPOOL_DEBUG 1
#else
#define THPOOL_DEBUG 0
#endif

class THPOOLException : public std::exception {
private: std::string message_;
public:
	THPOOLException(std::string & str) {
		message_ = str;
	}
	virtual const char* what() const throw() {
		return message_.c_str();
	}
};

#if !defined(DISABLE_PRINT) || defined(THPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str) throw THPOOLException(str)
#endif

//static volatile int threads_keepalive;
//static volatile int threads_on_hold;

#ifdef __cplusplus
extern "C" {
#endif


/* ========================== STRUCTURES ============================ */


/* Binary semaphore */
typedef struct bsem {
	pthread_mutex_t mutex;
	pthread_cond_t   cond;
	int v;
	bool mutex_inzed, cond_inzed;
} bsem;


/* Job */
typedef struct job{
	struct job*  prev;                   /* pointer to previous job   */
	void   (*function)(void* arg);       /* function pointer          */
	void*  arg;                          /* function's argument       */
	bsem*  signal_;
} job;


/* Job queue */
typedef struct jobqueue{
	pthread_mutex_t rwmutex;             /* used for queue r/w access */
	job  *front;                         /* pointer to front of queue */
	job  *rear;                          /* pointer to rear  of queue */
	bsem *has_jobs;                      /* flag as binary semaphore  */
	int   len;                           /* number of jobs in queue   */
	bool rwmutex_inzed;
} jobqueue;


/* Thread */
typedef struct thread{
	int          id;                    /* friendly id               */
	pthread_t pthread;                  /* pointer to actual thread  */
	struct thpool_* thpool_p;           /* access to thpool          */
} thread;


/* Threadpool */
typedef struct thpool_{
	thread**   threads;                  /* pointer to threads        */
	volatile int num_threads_alive;      /* threads currently alive   */
	volatile int num_threads_working;    /* threads currently working */
	pthread_mutex_t  thcount_lock;       /* used for thread count etc */
	pthread_cond_t  threads_all_idle;    /* signal to thpool_wait     */
	jobqueue  jobqueue;                  /* job queue                 */
	volatile int threads_keepalive;
	bool thcount_lock_inzed, threads_all_idle_inzed;
} thpool_;





/* ========================== PROTOTYPES ============================ */

static int   thread_init(thpool_* thpool_p, struct thread** thread_p, int id, int preffed_cpu);
static void* thread_do(void* thread_p);
static void  thread_hold(int sig_id);
static void  thread_destroy(struct thread* thread_p);

static int   jobqueue_init(jobqueue* jobqueue_p);
static void  jobqueue_clear(jobqueue* jobqueue_p);
static void  jobqueue_push(jobqueue* jobqueue_p, struct job* newjob_p);
static struct job* jobqueue_pull(jobqueue* jobqueue_p);
static void  jobqueue_destroy(jobqueue* jobqueue_p);

static void  bsem_init(struct bsem *bsem_p, int value);
static void  bsem_reset(struct bsem *bsem_p);
static void  bsem_post(struct bsem *bsem_p);
static void  bsem_post_all(struct bsem *bsem_p);
static void  bsem_wait(struct bsem *bsem_p);
static void  bsem_destroy(struct bsem *bsem_p);

static void  dec_bsem_init(struct bsem *bsem_p, int value);
static void  dec_bsem_post(struct bsem *bsem_p);
static void  dec_bsem_post_all(struct bsem *bsem_p);
static void  dec_bsem_wait(struct bsem *bsem_p);





/* ========================== THREADPOOL ===ifdef========================= */

int nprocs()
{
#ifdef LINUX
  cpu_set_t cs;
  CPU_ZERO(&cs);
  sched_getaffinity(0, sizeof(cs), &cs);
  return CPU_COUNT(&cs);
#else
  return 1;
#endif
}


/* Initialise thread pool */
struct thpool_* thpool_init(int num_threads){

	//threads_on_hold   = 0;
	//threads_keepalive = 1;

	if (num_threads < 0){
		num_threads = 0;
	}

	/* Make new thread pool */
	thpool_* thpool_p;
	thpool_p = (struct thpool_*)malloc(sizeof(struct thpool_));
	if (thpool_p == NULL){
		err("thpool_init(): Could not allocate memory for thread pool\n");
		return NULL;
	}
	thpool_p->num_threads_alive   = 0;
	thpool_p->num_threads_working = 0;
	thpool_p->threads_keepalive = 1;
	thpool_p->thcount_lock_inzed = false;
	thpool_p->threads_all_idle_inzed = false;

	/* Initialise the job queue */
	if (jobqueue_init(&thpool_p->jobqueue) == -1){
		err("thpool_init(): Could not allocate memory for job queue\n");
		free(thpool_p);
		return NULL;
	}

	/* Make threads in pool */
	thpool_p->threads = (struct thread**)malloc(num_threads * sizeof(struct thread *));
	if (thpool_p->threads == NULL){
		err("thpool_init(): Could not allocate memory for threads\n");
		jobqueue_destroy(&thpool_p->jobqueue);
		free(thpool_p);
		return NULL;
	}

	thpool_p->thcount_lock_inzed = pthread_mutex_init(&(thpool_p->thcount_lock), NULL) == 0;
	thpool_p->threads_all_idle_inzed = pthread_cond_init(&thpool_p->threads_all_idle, NULL) == 0;


    int n_of_threads = nprocs();
	/* Thread init */
	int n;
	int k = 0;
	for (n=0; n<num_threads; n++){
		thread_init(thpool_p, &thpool_p->threads[n], n, k);
		k++;
		if (k >= n_of_threads) k = 0;
#if THPOOL_DEBUG
			AddLog("THPOOL_DEBUG: Created thread %d in pool \n", n);
#endif
	}

	/* Wait for threads to initialize */
	while (thpool_p->num_threads_alive != num_threads) {}

	return thpool_p;
}


/* Add work to the thread pool */
int thpool_add_work(thpool_* thpool_p, void (*function_p)(void*), void* arg_p){
	job* newjob;

	newjob=(struct job*)malloc(sizeof(struct job));
	if (newjob==NULL){
		err("thpool_add_work(): Could not allocate memory for new job\n");
		return -1;
	}

	/* add function and argument */
	newjob->function=function_p;
	newjob->arg=arg_p;
	newjob->signal_ = NULL;

	/* add job to queue */
	jobqueue_push(&thpool_p->jobqueue, newjob);

	return 0;
}

/* Add work to the thread pool */
int thpool_add_work_with_sem(thpool_* thpool_p, bsem* signal_p, void (*function_p)(void*), void* arg_p){
	job* newjob;

	newjob=(struct job*)malloc(sizeof(struct job));
	if (newjob==NULL){
		err("thpool_add_work_and_wait(): Could not allocate memory for new job\n");
		return -1;
	}
	if (signal_p == NULL) {
		err("thpool_add_work_and_wait(): signal_p is NULL\n");
		return -2;
	}

	/* add function and argument */
	newjob->function=function_p;
	newjob->arg=arg_p;
	newjob->signal_ = signal_p;

	/* add job to queue */
	jobqueue_push(&thpool_p->jobqueue, newjob);

	return 0;
}


/* Wait until all jobs have finished */
void thpool_wait(thpool_* thpool_p){
	pthread_mutex_lock(&thpool_p->thcount_lock);
	while (thpool_p->jobqueue.len || thpool_p->num_threads_working) {
		pthread_cond_wait(&thpool_p->threads_all_idle, &thpool_p->thcount_lock);
		DO_SLEEP0ms;
	}
	pthread_mutex_unlock(&thpool_p->thcount_lock);
}

void thpool_wait_cond(bsem** el) {
	dec_bsem_wait(*el);
    bsem_destroy(*el);
}

void thpool_decsem_init(bsem** el, int value) {
	*el = (struct bsem*)malloc(sizeof(struct bsem));
	dec_bsem_init(*el, value);
}


/* Destroy the threadpool */
void thpool_destroy(thpool_* thpool_p){
	/* No need to destory if it's NULL */
	if (thpool_p == NULL) return ;

	volatile int threads_total = thpool_p->num_threads_alive;

	/* End each thread 's infinite loop */
	thpool_p->threads_keepalive = 0;

	/* Give one second to kill idle threads */
	double TIMEOUT = 1.0;
	time_t start, end;
	double tpassed = 0.0;
	time (&start);
	while (tpassed < TIMEOUT && thpool_p->num_threads_alive){
		bsem_post_all(thpool_p->jobqueue.has_jobs);
		time (&end);
		tpassed = difftime(end,start);
	}

	/* Poll remaining threads */
	while (thpool_p->num_threads_alive){
		bsem_post_all(thpool_p->jobqueue.has_jobs);
		DO_SLEEP1ms;
	}

	/* Job queue cleanup */
	jobqueue_destroy(&thpool_p->jobqueue);
	/* Deallocs */
	int n;
	for (n=0; n < threads_total; n++){
		thread_destroy(thpool_p->threads[n]);
	}
	if (thpool_p->thcount_lock_inzed) pthread_mutex_destroy(&(thpool_p->thcount_lock));
    if (thpool_p->threads_all_idle_inzed) pthread_cond_destroy(&(thpool_p->threads_all_idle));
	free(thpool_p->threads);
	free(thpool_p);
}


/* Pause all threads in threadpool */
void thpool_pause(thpool_* thpool_p) {
}


/* Resume all threads in threadpool */
void thpool_resume(thpool_* thpool_p) {
}


int thpool_num_threads_working(thpool_* thpool_p){
	return thpool_p->num_threads_working;
}





/* ============================ THREAD ============================== */


int stick_this_thread_to_core(pthread_t thread_p, int core_id) {
#ifdef LINUX
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   if (core_id < 0 || core_id >= num_cores)
      return EINVAL;

   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);

   return pthread_setaffinity_np(thread_p, sizeof(cpu_set_t), &cpuset);
#else
   return 0;
#endif // LINUX
}

/* Initialize a thread in the thread pool
 *
 * @param thread        address to the pointer of the thread to be created
 * @param id            id to be given to the thread
 * @preffed_cpu         preffered cpu num (-1 if not preffered)
 * @return 0 on success, -1 otherwise.
 */
static int thread_init (thpool_* thpool_p, struct thread** thread_p, int id, int preffed_cpu){

	*thread_p = (struct thread*)malloc(sizeof(struct thread));
	if (thread_p == NULL){
		err("thread_init(): Could not allocate memory for thread\n");
		return -1;
	}

	(*thread_p)->thpool_p       = thpool_p;
	(*thread_p)->id             = id;

	pthread_create(&(*thread_p)->pthread, NULL, thread_do, (*thread_p));
	if (preffed_cpu >= 0) {
        stick_this_thread_to_core((*thread_p)->pthread, preffed_cpu);
	}
	pthread_detach((*thread_p)->pthread);
	return 0;
}


/* Sets the calling thread on hold */
static void thread_hold(int sig_id) {
    //(void)sig_id;
	//threads_on_hold = 1;
	//while (threads_on_hold){
	//	sleep(1);
	//}
}


/* What each thread is doing
*
* In principle this is an endless loop. The only time this loop gets interuppted is once
* thpool_destroy() is invoked or the program exits.
*
* @param  thread        thread that will run this function
* @return nothing
*/
static void* thread_do(void * p0){
	struct thread* thread_p = (struct thread*)p0;
	/* Set thread name for profiling and debuging */
	//char thread_name[128] = {0};
	//AddLog(thread_name, "thread-pool-%d", thread_p->id);

#if defined(__linux__)
	/* Use prctl instead to prevent using _GNU_SOURCE flag and implicit declaration */
	#ifdef _ANDROID_COMPILER_
	prctl(PR_SET_NAME, thread_name);
	#endif
#elif defined(__APPLE__) && defined(__MACH__)
	pthread_setname_np(thread_name);
#else
	//err("thread_do(): pthread_setname_np is not supported on this system");
#endif

	/* Assure all threads have been created before starting serving */
	thpool_* thpool_p = thread_p->thpool_p;


	/* Mark thread as alive (initialized) */
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive += 1;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	while(thpool_p->threads_keepalive){

		bsem_wait(thpool_p->jobqueue.has_jobs);

		if (thpool_p->threads_keepalive){

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working++;
			pthread_mutex_unlock(&thpool_p->thcount_lock);

			/* Read job from queue and execute it */
			void (*func_buff)(void*);
			void*  arg_buff;
			job* job_p = jobqueue_pull(&thpool_p->jobqueue);
			if (job_p) {
				func_buff = job_p->function;
				arg_buff  = job_p->arg;
				func_buff(arg_buff);
				if (job_p->signal_) {
					dec_bsem_post(job_p->signal_);
				}
				free(job_p);
			}

			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working--;
			if (!thpool_p->num_threads_working) {
				pthread_cond_signal(&thpool_p->threads_all_idle);
			}
			pthread_mutex_unlock(&thpool_p->thcount_lock);

            DO_SLEEP0ms;
		}
	}
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive --;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	return NULL;
}


/* Frees a thread  */
static void thread_destroy (thread* thread_p){
	free(thread_p);
}


/* ============================ JOB QUEUE =========================== */


/* Initialize queue */
static int jobqueue_init(jobqueue* jobqueue_p){
	jobqueue_p->len = 0;
	jobqueue_p->front = NULL;
	jobqueue_p->rear  = NULL;
	jobqueue_p->rwmutex_inzed = NULL;

	jobqueue_p->has_jobs = (struct bsem*)malloc(sizeof(struct bsem));
	if (jobqueue_p->has_jobs == NULL){
		return -1;
	}

	jobqueue_p->rwmutex_inzed = pthread_mutex_init(&(jobqueue_p->rwmutex), NULL) == 0;
	bsem_init(jobqueue_p->has_jobs, 0);

	return 0;
}


/* Clear the queue */
static void jobqueue_clear(jobqueue* jobqueue_p){

	while(jobqueue_p->len){
		free(jobqueue_pull(jobqueue_p));
	}

	jobqueue_p->front = NULL;
	jobqueue_p->rear  = NULL;
	bsem_reset(jobqueue_p->has_jobs);
	jobqueue_p->len = 0;

}


/* Add (allocated) job to queue
 */
static void jobqueue_push(jobqueue* jobqueue_p, struct job* newjob){

	pthread_mutex_lock(&jobqueue_p->rwmutex);
	newjob->prev = NULL;

	switch(jobqueue_p->len){

		case 0:  /* if no jobs in queue */
					jobqueue_p->front = newjob;
					jobqueue_p->rear  = newjob;
					break;

		default: /* if jobs in queue */
					jobqueue_p->rear->prev = newjob;
					jobqueue_p->rear = newjob;

	}
	jobqueue_p->len++;

	bsem_post_all(jobqueue_p->has_jobs);
	pthread_mutex_unlock(&jobqueue_p->rwmutex);
}


/* Get first job from queue(removes it from queue)
<<<<<<< HEAD
 *
 * Notice: Caller MUST hold a mutex
=======
>>>>>>> da2c0fe45e43ce0937f272c8cd2704bdc0afb490
 */
static struct job* jobqueue_pull(jobqueue* jobqueue_p){

	pthread_mutex_lock(&jobqueue_p->rwmutex);
	job* job_p = jobqueue_p->front;

	switch(jobqueue_p->len){

		case 0:  /* if no jobs in queue */
		  			break;

		case 1:  /* if one job in queue */
					jobqueue_p->front = NULL;
					jobqueue_p->rear  = NULL;
					jobqueue_p->len = 0;
					break;

		default: /* if >1 jobs in queue */
					jobqueue_p->front = job_p->prev;
					jobqueue_p->len--;

	}

	if (jobqueue_p->len == 0) {
        pthread_mutex_lock(&jobqueue_p->has_jobs->mutex);
        jobqueue_p->has_jobs->v = 0;
        pthread_mutex_unlock(&jobqueue_p->has_jobs->mutex);
	} else {
        /* more than one job in queue -> post it */
		bsem_post_all(jobqueue_p->has_jobs);
	}

	pthread_mutex_unlock(&jobqueue_p->rwmutex);
	return job_p;
}


/* Free all queue resources back to the system */
static void jobqueue_destroy(jobqueue* jobqueue_p){
	jobqueue_clear(jobqueue_p);
	if (jobqueue_p->rwmutex_inzed) pthread_mutex_destroy(&(jobqueue_p->rwmutex));
	bsem_destroy(jobqueue_p->has_jobs);
}





/* ======================== SYNCHRONISATION ========================= */


/* Init semaphore to 1 or 0 */
static void bsem_init(bsem *bsem_p, int value) {
    bsem_p->mutex_inzed = false;
    bsem_p->cond_inzed = false;
	if (value < 0 || value > 1) {
		err("bsem_init(): Binary semaphore can take only values 1 or 0");
		exit(1);
	}
	bsem_p->mutex_inzed = pthread_mutex_init(&(bsem_p->mutex), NULL) == 0;
	bsem_p->cond_inzed = pthread_cond_init(&(bsem_p->cond), NULL) == 0;
	bsem_p->v = value;
}

/* Reset semaphore to 0 */
static void bsem_reset(bsem *bsem_p) {
    if (bsem_p->cond_inzed) pthread_cond_destroy(&(bsem_p->cond));
    if (bsem_p->mutex_inzed) pthread_mutex_destroy(&(bsem_p->mutex));
	bsem_init(bsem_p, 0);
}

/* Post to at least one thread */
static void bsem_post(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_signal(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Post to all threads */
static void bsem_post_all(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_broadcast(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Wait on semaphore until semaphore has value 0 */
static void bsem_wait(bsem* bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	while (bsem_p->v != 1) {
		pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
	}
	// bsem_p->v = 0;
	pthread_mutex_unlock(&bsem_p->mutex);
}

static void  bsem_destroy(struct bsem *bsem_p) {
    if (bsem_p->cond_inzed) pthread_cond_destroy(&(bsem_p->cond));
    if (bsem_p->mutex_inzed) pthread_mutex_destroy(&(bsem_p->mutex));
    free(bsem_p);
}

/* Init semaphore to 1 or 0 */
static void dec_bsem_init(bsem *bsem_p, int value) {
    bsem_p->mutex_inzed = false;
    bsem_p->cond_inzed = false;
	if (value < 0) {
		err("dec_bsem_init(): Dec semaphore can take only values gequal 0");
		exit(1);
	}
	bsem_p->mutex_inzed = pthread_mutex_init(&(bsem_p->mutex), NULL) == 0;
	bsem_p->cond_inzed = pthread_cond_init(&(bsem_p->cond), NULL) == 0;
	bsem_p->v = value;
}

/* Post to at least one thread */
static void dec_bsem_post(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v--;
	if (bsem_p->v == 0) pthread_cond_signal(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Post to all threads */
static void dec_bsem_post_all(bsem *bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v--;
	pthread_cond_broadcast(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}

/* Wait on semaphore until semaphore has value 0 */
static void dec_bsem_wait(bsem* bsem_p) {
	pthread_mutex_lock(&bsem_p->mutex);
	while (bsem_p->v) {
		pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
	}
	bsem_p->v = 0;
	pthread_mutex_unlock(&bsem_p->mutex);
}

#ifdef __cplusplus
}
#endif
