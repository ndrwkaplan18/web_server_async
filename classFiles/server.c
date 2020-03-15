#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#define VERSION 25
#define BUFSIZE 8096
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*TYPEDEF, STRUCT, AND GLOBAL DECLARATIONS */
struct {
	char *ext;
	char *filetype;
} extensions [] = {
	{"gif", "image/gif" },

	{"jpg", "image/jpg" },
	{"jpeg","image/jpeg"},
	{"png", "image/png" },
	{"ico", "image/ico" },
	{"zip", "image/zip" },
	{"gz",  "image/gz"  },
	{"tar", "image/tar" },
	{"htm", "text/html" },
	{"html","text/html" },
	{0,0} }; // extensions[5].filetype//come back to

static const char * HDRS_FORBIDDEN = "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n";
static const char * HDRS_NOTFOUND = "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n";
static const char * HDRS_OK = "HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n";
static int dummy; //keep compiler happy
static char THERE_IS_NO_WORK_TO_BE_DONE, SHOULD_WAKE_UP_THE_PRODUCER, THE_BUFFER_IS_FULL;  

/* what a worker thread needs to start a job */
typedef struct {
	int job_id;
	int job_fd; // the socket file descriptor
	int taken;//tells the producer whether or not this job was taken or not. 
	int type;// 1 for pic, 0 for Text.
	char * first_part; // When we find out the type, we consume part of the file which is saved here
	// what other stuff needs to be here eventually?
} job_t;

typedef struct {
	job_t * jobBuffer; // array of server Jobs on heap
	size_t buf_capacity;
	size_t head; // position of writer
	size_t tail; // position of reader
	pthread_mutex_t work_mutex;
	pthread_cond_t c_cond; // P/C condition variables
	pthread_cond_t p_cond;
	char* schedalg;
	int textFiles;
	int fileCounter;
	int picFiles;
	
} tpool_t;


// define type for worker thread C function
typedef void * (worker_fn) (void *);

static tpool_t the_pool; // one pool to rule them all
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/* FUNCTION DECLARATIONS */
// Thread pool functions
void tpool_init(tpool_t *tm, size_t num_threads, size_t buf_size, worker_fn *worker,char* schedalg);
static void *tpool_worker(void *arg);
char tpool_add_work(job_t job);

// Thread pool helper functions
job_t REMOVE_JOB_FROM_BUFFER();
job_t REMOVE_FIFO_JOB_FROM_BUFFER();
job_t REMOVE_PIC_JOB_FROM_BUFFER();
job_t REMOVE_TXT_JOB_FROM_BUFFER();
void ADD_JOB_TO_BUFFER(job_t job);
void DO_THE_WORK(job_t *job);
void getFileExtension(job_t *job);
// The work
void logger(int type, char *s1, char *s2, int socket_fd);
void web(int fd, int hit, char * first_part);
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*HELPER FUNCTIONS */
void getFileExtension(job_t *job){//returns 1 for images and 0 for Text
	int i, j, k;
	int p;
    static char buffer[BUFSIZE+1]; /* static so zero filled */
	i = 0, k = 0;
	char c;
	// Read until the 2nd space. Assuming request format is "GET <path> <headers>"
	for(;i < 2;){
		//TODO check status of sys call
		if((p = read(job->job_fd, &buffer[k], 1)) != 0){ // Read 1 byte from job_fd into buffer[k]
			if(buffer[k] ==  ' ') i++;
		}
		else fprintf(stderr, "failed to read from fd. error code %d\n", p);
		k++;	
	}
	// fprintf(stdout,"%s",buffer);
	// GET /index.html blah blah
	// Seek backwards to the '.' denoting the file extension
	while(buffer[k--] != '.');
	i = 0;
	// Fill in the extension with the characters up to and not including the final space
	char ext[20];
	while((c = buffer[++k]) != ' ')
		ext[++i] = c;
	job->first_part = buffer;
	fprintf(stdout,"\nJOB>FIRST PART: %s\n",job->first_part);
	for (j=0;j<=7; j++){//The first 8 files in the ext array are images.
		if(!strcmp(ext,extensions[j].ext)){
			job->type = 1;
			return;
		}
	}
	job->type =0;
	return;
}

job_t REMOVE_JOB_FROM_BUFFER(){
	tpool_t *tm = &the_pool;
	job_t job;
	
	if(!strcmp(tm->schedalg, "HPIC")){ 
		job =  REMOVE_PIC_JOB_FROM_BUFFER(tm);
	return job;
	}
	if(!strcmp(tm->schedalg, "HPHC")){
		 job = REMOVE_TXT_JOB_FROM_BUFFER(tm);	
	return job;
	}
	else{
		job = REMOVE_FIFO_JOB_FROM_BUFFER(tm);	
	return job;
	}
}

job_t REMOVE_FIFO_JOB_FROM_BUFFER(){
	// Return job currently pointed to by tail ptr, then decrement tail ptr
	tpool_t *tm = &the_pool;
	job_t job;
	job = tm->jobBuffer[tm->tail];
	tm->tail = (tm->tail + 1) % tm->buf_capacity;
	fprintf(stdout, "In REMOVE_JOB_FROM_BUFFER. Taking job %d. tail was %d now is %d\n", job.job_id, (int) tm->head - 1, (int)tm->head);
	// Test here if the buffer is empty, if so set THERE_IS_NO_WORK_TO_BE_DONE and SHOULD_WAKE_UP_THE_PRODUCER to 1
	if(tm->tail == tm->head){
		THERE_IS_NO_WORK_TO_BE_DONE = 1;
		SHOULD_WAKE_UP_THE_PRODUCER = 1;
	}
	THE_BUFFER_IS_FULL = 0;
	return job;
}
job_t REMOVE_PIC_JOB_FROM_BUFFER(){
	fprintf(stdout, "\nin Remove Pic Job from BUffer\n");
	job_t job;
	int tempPicFiles=1;//Keeps track if therea re any pics in the buffer
	tpool_t *tm = &the_pool;
	int availableTxtfileIndex;
	/*Checks if theres any work Left*/
	if(tm->fileCounter == 0){
		THERE_IS_NO_WORK_TO_BE_DONE = 1;
		SHOULD_WAKE_UP_THE_PRODUCER = 1;
	}
	if(tm->fileCounter != 0){ 
		for(int i =0; i< tm->buf_capacity; i++){//loop through the buffer	
				if(tm->picFiles==0){//First check if there are Pics left-If not, we get the index of a textFile
					if(tm->jobBuffer[i].taken == 0){//We get the index of an available Text File.
						availableTxtfileIndex = i;
						tempPicFiles =0;
						break;
					}	
				}
				if((job.type == 1)&&(tm->jobBuffer[i].taken == 0)){ //Any of the AVAILABLE pics Files.
				tm->jobBuffer[i].taken = 1;
				tm->picFiles--;//decrement the picfiles amount
				job = tm->jobBuffer[i];//Return the pic
				}
			}
	}
	if(tempPicFiles ==0){//there are no pics, so we just do a normal remove-
			job = tm->jobBuffer[availableTxtfileIndex];//Return the text.
			tm->textFiles--;//decrement the text Files.
			tm->jobBuffer[availableTxtfileIndex].taken = 1;
	}
	THE_BUFFER_IS_FULL = 0;//either way we know its not full.
	return job;//whichever one it is
}
job_t REMOVE_TXT_JOB_FROM_BUFFER(){
	fprintf(stdout, "\nin Remove Text JOb from BUffer\n");
	job_t job;
	int tempTxtFiles=1;//Keeps track if therea re any pics in the buffer
	tpool_t *tm = &the_pool;
	int availablePicfileIndex;
	/*Checks if theres any work Left*/
	if(tm->fileCounter == 0){
		THERE_IS_NO_WORK_TO_BE_DONE = 1;
		SHOULD_WAKE_UP_THE_PRODUCER = 1;
	}
	if(tm->fileCounter != 0){ 
		for(int i =0; i< tm->buf_capacity; i++){//loop through the buffer	
				if(tm->textFiles==0){//First check if there are Texts left-If not, we get the index of a PicFile
					if(tm->jobBuffer[i].taken == 0){//We get the index of an available Pic File.
						availablePicfileIndex = i;
						tempTxtFiles =0;
						break;
					}	
				}
				if((job.type == 0)&&(tm->jobBuffer[i].taken == 0)){ //Any of the AVAILABLE text Files.
				tm->jobBuffer[i].taken = 1;
				tm->textFiles--;//decrement the textfiles amount
				job = tm->jobBuffer[i];//Return the pic
				}
			}
	}
	if(tempTxtFiles ==0){//there are no pics, so we just do a normal remove-
			job = tm->jobBuffer[availablePicfileIndex];//Return the text.
			tm->picFiles--;//decrement the text Files.
			tm->jobBuffer[availablePicfileIndex].taken = 1;
	}
	THE_BUFFER_IS_FULL = 0;//either way we know its not full.
	return job;//whichever one it is
}
void ADD_JOB_TO_BUFFER(job_t job){
	// add job to that index and increment the pointer.
	tpool_t *tm = &the_pool;
	tm->jobBuffer[tm->head] = job;
	tm->head = (tm->head + 1) % tm->buf_capacity;
	THERE_IS_NO_WORK_TO_BE_DONE = 0;
	SHOULD_WAKE_UP_THE_PRODUCER = 0;
	tm->fileCounter++;
	if(job.type == 1){
		tm->picFiles++;
	}
	if(job.type == 0){
		tm->textFiles++;
	}
	if((tm->head + 1 % tm->buf_capacity) == tm->tail)
		THE_BUFFER_IS_FULL = 1;
	fprintf(stdout, "\nIn ADD_JOB_TO_BUFFER. Adding job %d.\nhead was %d now is %d\n", job.job_id, (int) tm->head - 1, (int)tm->head);
	
}

void DO_THE_WORK(job_t *job){
	fprintf(stdout,"\nDoing JOB %d\n",job->job_id);
	web(job->job_fd, job->job_id, job->first_part);//
}
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*THREAD POOL FUNCTIONS */
void tpool_init(tpool_t *tm, size_t num_threads, size_t buf_size, worker_fn *worker, char* schedalg)
{
	pthread_t* threads = (pthread_t*) malloc(sizeof(pthread_t));
	size_t	i;
	int status;

	pthread_mutex_init(&(tm->work_mutex), NULL);
	pthread_cond_init(&(tm->p_cond), NULL);
	pthread_cond_init(&(tm->c_cond), NULL);

	// initialize buffer to empty condition
	if (!strcmp(schedalg,"HPIC")){
		tm->schedalg = "HPIC";
	}
	else if(!strcmp(schedalg,"HPHC")){
		tm->schedalg = "HPHC";
	}
	else{
		tm->schedalg = "FIFO";
	}
	
	tm->head = tm->tail = 0;
	tm->buf_capacity = buf_size;
	tm->jobBuffer = (job_t*) calloc(buf_size, sizeof(job_t));
	THERE_IS_NO_WORK_TO_BE_DONE = 1;
	THE_BUFFER_IS_FULL = 0;
	SHOULD_WAKE_UP_THE_PRODUCER = 0;

    for (i=0; i<num_threads; i++) {
		if((status = pthread_create(&threads[i], NULL, *worker, (void *) (i + 1))) == 0){
			pthread_detach(threads[i]); // make non-joinable
		}
		else
			fprintf(stderr, "Oops, pthread_create() returned error code %d when attempting to make thread %d\n",status, (int) i);
	}
}


static void *tpool_worker(void *arg){
	tpool_t *tm = &the_pool;
	int my_id = (intptr_t) arg; // Just casting to (int) triggers warning: "cast to pointer from integer of different size"
	// https://stackoverflow.com/questions/21323628/warning-cast-to-from-pointer-from-to-integer-of-different-size
	// printf("Hello from thread %d!\n",my_id);
	while (1) {
		job_t *job = (job_t*) malloc(sizeof(job_t));//creates an array of Jobs
		pthread_mutex_lock(&(tm->work_mutex));
		while (THERE_IS_NO_WORK_TO_BE_DONE){
			// pthread_cond_signal(&tm->p_cond);
			pthread_cond_wait(&(tm->c_cond), &(tm->work_mutex));
		}
		*job = REMOVE_JOB_FROM_BUFFER(tm);
		fprintf(stdout, "Hello from thread %d! Doing job %d now.\n",my_id, (int) job->job_id);
		pthread_mutex_unlock(&(tm->work_mutex));
		
		DO_THE_WORK(job);  // call web() plus ??
		pthread_mutex_lock(&(tm->work_mutex));
		if (SHOULD_WAKE_UP_THE_PRODUCER)
			pthread_cond_signal(&(tm->p_cond));
		pthread_mutex_unlock(&(tm->work_mutex));
	}
	return NULL;
}

char tpool_add_work(job_t job){
	tpool_t *tm = &the_pool;
	getFileExtension(&job);
	pthread_mutex_lock(&(tm->work_mutex));
	while (THE_BUFFER_IS_FULL)
		pthread_cond_wait(&(tm->p_cond), &(tm->work_mutex));
	ADD_JOB_TO_BUFFER(job);

	// Wake the Keystone Cops!! (improve this eventually)
	// fprintf(stdout, "Broadcasting to consumer\n");
	pthread_cond_broadcast(&(tm->c_cond));
	pthread_mutex_unlock(&(tm->work_mutex));

	return 1;
}
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*SERVER FUNCTIONS */
/*
Based on what the client supplies, Logger will either return one of several Error-type messages, or open and write to "nweb.log".
*/
void logger(int type, char *s1, char *s2, int socket_fd)
{
	int fd;
	char logbuffer[BUFSIZE*2];

	switch (type) {
	case ERROR: (void)sprintf(logbuffer,"ERROR: %s:%s Errno=%d exiting pid=%d",s1, s2, errno,getpid());
		break;
	case FORBIDDEN:
		dummy = write(socket_fd, HDRS_FORBIDDEN,271);
		(void)sprintf(logbuffer,"FORBIDDEN: %s:%s",s1, s2);
		break;
	case NOTFOUND:
		dummy = write(socket_fd, HDRS_NOTFOUND,224);
		(void)sprintf(logbuffer,"NOT FOUND: %s:%s",s1, s2);
		break;
	case LOG: (void)sprintf(logbuffer," INFO: %s:%s:%d",s1, s2,socket_fd); break;
	}
	/* No checks here, nothing can be done with a failure anyway */
	if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
		dummy = write(fd,logbuffer,strlen(logbuffer));
		dummy = write(fd,"\n",1);
		(void)close(fd);
	}
}

/* this is a child web server process, so we can exit on errors */
void web(int fd, int hit, char * first_part)
{
	int j, file_fd, buflen;
    long i, ret, len;
    char * fstr;
    static char buffer2[BUFSIZE+1]; /* static so zero filled */
	static char buffer[BUFSIZE+1];
	
	fprintf(stdout,"In web. first half of buffer: %s\n", first_part);
	for (int i =0; i < BUFSIZE;i++){
		if((buffer[i]== ' ')&&(buffer[i-1]== ' ')){
			break;
		}
		buffer[i] = first_part[i];
	}

    ret =read(fd,buffer2,BUFSIZE);   /* read Web request in one go */
	strcat(buffer, buffer2);
	fprintf(stdout,"In web. full request is:\n%s\n", buffer);
	// concat here first_part + buffer
    if(ret == 0 || ret == -1) { /* read failure stop now */
		
		logger(FORBIDDEN,"failed to read browser request","",fd);
        goto endRequest;
    }
    if(ret > 0 && ret < BUFSIZE) {  /* return code is valid chars */
        buffer[ret]=0;      /* terminate the buffer */
    }
    else {
        buffer[0]=0;
    }
    for(i=0;i<ret;i++) {    /* remove CF and LF characters */
        if(buffer[i] == '\r' || buffer[i] == '\n') {
            buffer[i]='*';
        }
    }
    logger(LOG,"request",buffer,hit);
    if( strncmp(buffer,"GET ",4) && strncmp(buffer,"get ",4)) { //!!וביה מניה סתירה !והא
        logger(FORBIDDEN,"Only simple GET operation supported",buffer,fd);
        goto endRequest;
    }
    for(i=4;i<BUFSIZE-20;i++) { /* null terminate after the second space to ignore extra stuff */
        if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
            buffer[i] = 0;
            break;
        }
    }
	// Now i = BUFSIZE - 1
	// URL: https://www.tutorialspoint.com/../../../c_standard_library/c_function_strncmp.htm
    for(j=0;j<i-1;j++) {    /* check for illegal parent directory use .. */
        if(buffer[j] == '.' && buffer[j+1] == '.') {
            logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,fd);
            goto endRequest;
        }
    }
    if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) { /* convert no filename to index file */
        (void)strcpy(buffer,"GET /index.html");
    }

    /* work out the file type and check we support it */
    buflen=strlen(buffer);
    fstr = (char *)0;
    for(i=0;extensions[i].ext != 0;i++) {
        len = strlen(extensions[i].ext);
        if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
            fstr = extensions[i].filetype;
            break;
        }
    }
    if(fstr == 0){
        logger(FORBIDDEN,"file extension type not supported",buffer,fd);
    } // GET /zoobat.jpg
    if(( file_fd = open(&buffer[5],O_RDONLY)) == -1) {  /* open the file for reading */
        logger(NOTFOUND, "failed to open file",&buffer[5],fd);
        goto endRequest;
    }
    logger(LOG,"SEND",&buffer[5],hit);
    len = (long)lseek(file_fd, (off_t)0, SEEK_END); /* lseek to the file end to find the length */
          (void)lseek(file_fd, (off_t)0, SEEK_SET); /* lseek back to the file start ready for reading */
          /* print out the response line, stock headers, and a blank line at the end. */
          (void)sprintf(buffer, HDRS_OK, VERSION, len, fstr);
    logger(LOG,"Header",buffer,hit);
    dummy = write(fd,buffer,strlen(buffer));
	
    /* send file in 8KB block - last block may be smaller */
    while ( (ret = read(file_fd, buffer, BUFSIZE-20)) > 0 ) {
        dummy = write(fd,buffer,ret);
    }
    endRequest:
    sleep(1);   /* allow socket to drain before signalling the socket is closed */
    close(fd);
}
/************************************************************************************************************************************/
/************************************************************************************************************************************/
/*MAIN */
int main(int argc, char **argv)
{
	int i, port, listenfd, socketfd, hit;
	socklen_t length;
	static struct sockaddr_in cli_addr; /* static = initialised to zeros */
	static struct sockaddr_in serv_addr; /* static = initialised to zeros */
	worker_fn *worker = tpool_worker;
	tpool_t *tm = &the_pool;
	job_t job;

	if( argc < 6  || argc > 6 || !strcmp(argv[1], "-?") ) {
		(void)printf("USAGE: %s <port-number> <top-directory>\t\tversion %d\n\n"
	"\tnweb is a small and very safe mini web server\n"
	"\tnweb only servers out file/web pages with extensions named below\n"
	"\t and only from the named directory or its sub-directories.\n"
	"\tThere is no fancy features = safe and secure.\n\n"
	"\tExample: nweb 8181 /home/nwebdir &\n\n"
	"\tOnly Supports:", argv[0], VERSION);
		for(i=0;extensions[i].ext != 0;i++)
			(void)printf(" %s",extensions[i].ext);

		(void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
	"\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
	"\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n"  );
		exit(0);
	}
	if( !strncmp(argv[2],"/"   ,2 ) || !strncmp(argv[2],"/etc", 5 ) ||
	    !strncmp(argv[2],"/bin",5 ) || !strncmp(argv[2],"/lib", 5 ) ||
	    !strncmp(argv[2],"/tmp",5 ) || !strncmp(argv[2],"/usr", 5 ) ||
	    !strncmp(argv[2],"/dev",5 ) || !strncmp(argv[2],"/sbin",6) ){
		(void)printf("ERROR: Bad top directory %s, see nweb -?\n",argv[2]);
		exit(3);
	}
	if(chdir(argv[2]) == -1){
		(void)printf("ERROR: Can't Change to directory %s\n",argv[2]);
		exit(4);
	}
	logger(LOG,"nweb starting",argv[1],getpid());
	/* setup the network socket */
	if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0){
		logger(ERROR, "system call","socket",0);
	}
	port = atoi(argv[1]);
	if(port < 1025 || port >65000) {
		logger(ERROR,"Invalid port number (try 1025->65000)",argv[1],0);
	}
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);
	if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0){
		logger(ERROR,"system call","bind",0);
	}
	if( listen(listenfd,64) <0) {
		logger(ERROR,"system call","listen",0);
	}
	fprintf(stdout, "port: %d\nnumthreads: %d\nbufsize: %d\n",atoi(argv[1]),atoi(argv[3]),atoi(argv[4]));
	// Set up thread pool
	char* schedalg = argv[4];
	tpool_init(tm, atoi(argv[3]), atoi(argv[4]), *worker, schedalg);
	
	for(hit=1; ;hit++) {
		length = sizeof(cli_addr);
		if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0) {
			logger(ERROR,"system call","accept",0);
		}
		job.taken = 0;
		job.job_fd = socketfd;
		job.job_id = hit;
		tpool_add_work(job);
	}
}
/* Step one: Main makes a pool, a job and the worker.
* 	Step two: Main adds Job to the Pool BUffer
* Step three: There are going to be jobs coming through from client- go to worker
* step four: Worker takes it out of Buffer in the directed order
* 			A. FiFO- JUST Takes it out.
			B. Other two: we have to go through the buffer and rreturn the job with the correct ext.
				How to find the ext?
					RemoveJob(s) call getEXT() and suppy the job info. we open the file, get the ext, and return an
					int pertaining to the extension. 1 for pic, 0 for Txt.
k

Step 5: Does the job, and logger and etcettera.*/