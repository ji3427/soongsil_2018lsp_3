#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>

#define MODE_A O_RDWR|O_APPEND|O_CREAT
#define MODE_R O_RDWR
#define MODE_W O_RDWR|O_CREAT|O_TRUNC

typedef struct _psinfo{
	int pid;
	char name[100];
	char state;
}ps_info;

typedef struct _file_list{
	char file_name[256];
	struct _file_list * next;
}file_list;

int get_filetype(char*);
int ssu_daemon_init(void);
char* to_abs_path(char * path);
char* to_hex_adress(char * path);
char* hex_to_origin(char * path);
int is_run(char*);
ps_info* get_stat();
void backup(char*,char*);
char* get_current_time();
void sig_handler(int signo);
char *path_basename(char* path);
void list_add(file_list * head,char* fname);
int get_list_size(file_list * head);
void backup_handler(int flag_n,int n_num,int flag_d,char * fname,int period);
void * ssu_thread(void* arg);
char* address_add(char * s1, char* s2);
void list_file_add(file_list * head , char* file_name);
file_list* get_node(int index,file_list * head);
char* getdir(char * path);
char* get_hex_filename(char * hex_name);
int get_dir_file_count(char * path);
char* get_latest_file(char * fname);

char * log_path;
char * target_path;
char * backup_dir_path;
char * debug_path;
int log_fd;
int target_fd;
int debug_fd;
int flag_d=0,flag_r=0,flag_m=0,flag_n=0,flag_c=0;
int n_num = 0;
int period;
char* stradd(char * s1, char* s2);
void restore();
void compare();
pthread_mutex_t mutx = PTHREAD_MUTEX_INITIALIZER;
time_t start_time;
int main(int argc, char * argv[])
{
	start_time = time(NULL); 
	log_path = to_abs_path("backup_log.txt");
	target_path = to_abs_path(argv[1]);
	backup_dir_path = to_abs_path("backup_dir");
	debug_path = to_abs_path("debug.txt");
	log_fd = open(log_path,MODE_A,0644);
	signal(SIGUSR1,sig_handler); //SIGUSR1이 들어온 경우 sig_handler실행
	char buf[1024];
	int option;
	int count;
	target_fd = open(target_path,MODE_R);
	int run_state = is_run("ssu_backup"); //프로세스가 실행중인지 아닌지 검사 , 실행중일경우 pid, 아닐경우 -1 return
	if(run_state != -1){ //프로세스가 실행중인 경우
		printf("send signal to ssu_backup process<%d>\n",run_state);
		sprintf(buf,"%d process exit %s\n",run_state,get_current_time());
		write(log_fd,buf,strlen(buf));
		kill(run_state,SIGUSR1); //실행중인 process에 signal을 보냄
	}
	if(access(target_path,F_OK)<0){ //파일이 존재하지 않는 경우
		fprintf(stderr,"file not exist\n");
		exit(1);
	}
	if(argv[2][0] == '-'){ //period가 안오는 옵션의 경우
		if(argc != 3){
			fprintf(stderr,"argument error\n");
			exit(1);
		}
	}
	else{
		period = atoi(argv[2]);
		if(period > 10 || period < 3){ //period의 범위가 이상한 경우 종료
			fprintf(stderr,"period is too short or too long\n");
			exit(1);
		}
	}
	mkdir(backup_dir_path,0777); //backupdir 폴더 생성
	while ((option = getopt(argc,argv,"drmn:c")) != -1){ //각각의 옵션에 맞게 flag를 설정함
		switch(option){
			case 'd':
				flag_d = 1;
				break;
			case 'r':
				flag_r = 1;
				break;
			case 'm': 
				flag_m = 1;
				break;
			case 'n':
				flag_n = 1;
				n_num = atoi(optarg);
				if(n_num < 1){
					fprintf(stderr,"wrong n number\n");
					exit(1);
				}
				break;
			case 'c':
				flag_c = 1;
				break;
			case '?':
				exit(1);
			}
	}
	if(flag_d == 1){
		if(get_filetype(target_path) != 2){ //d옵션이 왔는데 디렉토리가 아닌 경우
			fprintf(stderr,"file not directory\n");
			exit(1);
		}
		count =get_dir_file_count(target_path); //디렉토리일 경우 현재 디렉토리 내에 파일 개수를 저장(새로운 파일의 생성을 검사하기 위해서)
	}
	else{
		if(get_filetype(target_path) != 1){ //d옵션이 오지않았는데 일반파일이 아닌경우
			fprintf(stderr,"file not regular file\n");
			exit(1);
		}
	}
	if(flag_r == 1){ //r옵션인 경우
		restore();
		exit(0);
	}
	if(flag_c == 1){ //c옵션인 경우
		compare();
		exit(0);
	}
	ssu_daemon_init(); //디몬프로세스로 설정
	log_fd = open(log_path,MODE_A,0644);
	target_fd = open(target_path,MODE_R);
	debug_fd = open(debug_path,MODE_A,0644);
	backup_handler(flag_n,n_num,flag_d,target_path,period);
	while(1){ // 파일이 새로 생겼는지 안생겼는지 검사하는 부분
		if(flag_d == 1 && get_dir_file_count(target_path) > count){ //d옵션이 왔으면서 기존 디렉토리내의 파일 개수보다 파일이 많아진 경우
			sprintf(buf,"%s modified(new %s file make)\n",path_basename(target_path),path_basename(get_latest_file(target_path))); //log에 입력
            write(log_fd,buf,strlen(buf));
			pthread_t tid;
			pthread_create(&tid, NULL, ssu_thread,(void*)get_latest_file(target_path)); //새 파일에 대한 새로운 쓰레드 생성
			count = get_dir_file_count(target_path);
		}
		sleep(2);//2초마다 검사
	}
	
}

int ssu_daemon_init(void){ //디몬프로세스로 설정해주는 함수
	pid_t pid;
	int fd, maxfd;
	
	if ((pid = fork()) < 0) {
		fprintf(stderr,"fork error\n");
		exit(1);
	}
	else if(pid != 0)
		exit(0);
		
	pid = getpid();
	printf("process %d running as daemon\n",pid);
	setsid();
	signal(SIGTTIN,SIG_IGN);
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTSTP,SIG_IGN);
	maxfd = getdtablesize();
	for(fd = 0 ; fd < maxfd ; fd++)
		close(fd);
	umask(0);
	chdir("/");
	fd = open("/dev/null",O_RDWR);
	dup(0);
	dup(0);
	return 0;
}


char* to_abs_path(char * path){ //상대경로를 절대경로로 바꿔주는 함수 (경로 끝에 '/' 안 붙음)
	char *abs_path = malloc(PATH_MAX);
	if(path[0] == '/'){
		return path;
	}
	else{
		getcwd(abs_path,PATH_MAX);
		abs_path[strlen(abs_path)] ='/';
		abs_path[strlen(abs_path)+1] ='\0';
		strcat(abs_path,path);
		return abs_path;
	}
}

char* to_hex_adress(char * path){ //16진수로 바꿔주는 함수
	char * origin = malloc(sizeof(char)*300); 
	strcpy(origin, getcwd(NULL,0)); //기존의 작업디렉토리 저장
	char * temp = malloc(sizeof(char)*300);
	strcpy(temp,path);
	char * abs_path = to_abs_path(temp); //기존의 경로를 절대경로로 바꿈
	//파일명으로 들어오는 "." 혹은 ".." 을 처리하기 위해 chdir -> getcwd를 통해서 바꿔줌
	if(get_filetype(abs_path) == 1){ //일반파일인 경우
		chdir(getdir(abs_path)); // 일반파일이 존재하는 디렉토리로 작업디렉토리 변경
		strcpy(abs_path,getcwd(NULL,0));
		strcat(abs_path,"/");
		strcat(abs_path,path_basename(path)); //절대경로로 바꿔줌
	}
	else{ //디렉토리인 경우
		chdir(abs_path);
		strcpy(abs_path,getcwd(NULL,0));
	}
	chdir(origin); //작업디렉토리 복구
	char * hex_path = malloc(PATH_MAX);
	int j = 0;
	j = sprintf(hex_path, "%x",abs_path[0]); //1글자씩 16진수로 바꾸어서 저장
	for(int i = 1 ; i < strlen(temp) ; i++){
		if(j > 244){
			syslog(LOG_ERR,"file name is too long");
			exit(1);
		}
		j += sprintf(hex_path+j,"%x",abs_path[i]);
	}
	return hex_path;
}

char* hex_to_origin(char * hex_path){ //16진수를 원래 이름으로 바꾸는 함수
	int i = 0;
	int hex;
	char temp[4];
	temp[0] = '0';
	temp[1] = 'x';
	char * seg = malloc(sizeof(char) * 10);
	char * result = malloc(sizeof(char)*200);
	strcpy(result,"");
	strcpy(seg,"");
	while(hex_path[i] != '_'){ //날짜가 나오기 전까지 읽어들임
		temp[2]=hex_path[i];
		temp[3]=hex_path[i+1];//2글자씩 읽어들여 16진수화 시킴
		hex = strtol(temp,NULL,16); //16진수 문자열을 long으로 변환
		sprintf(seg,"%c",hex);
		strcat(result,seg);
		i=i+2;
	}
	strcat(result,&hex_path[i]); //마지막 날짜부분까지 붙어줌
	return result;
}
//실행중인 프로세스가 존재할 경우 실행중인 프로세스의 pid return, 그 외에 경우 -1 return
int is_run(char * ps_name){
	chdir("/proc");
	ps_info * pi;
	struct dirent **namelist;
	int count;
	if((count = scandir("/proc", &namelist, NULL, alphasort)) == -1){ //proc 폴더 내의 파일들을 읽어들임
		fprintf(stderr,"scandir error\n");
		exit(1);
	}
	for(int j = 0 ; j < count ; j++){
		if(strcmp(namelist[j]->d_name,".")==0 || strcmp(namelist[j]->d_name,"..")==0){
			continue;
		}
		if(get_filetype(namelist[j]->d_name) == 2){ //폴더일 경우
			chdir(namelist[j]->d_name);
			pi=get_stat(); //stat파일을 통해서 process 정보를 가지고 옴
			chdir("..");
			if(pi == NULL){
				continue;
			}
			else{
				if(strcmp(ps_name,pi->name) == 0 && (pi->state =='R' || pi->state =='S') && pi->pid != getpid()){ //ssu_backup의 이름을 가지면서 R 혹은 S 상태인 프로세스가 존재하는 경우
					return pi->pid;
				}
			}
		}
	}
	return -1;
}

int get_filetype(char * file){ //파일타입 파악 함수
	struct stat stat_buf;
	if(!access(file,F_OK)){
		lstat(file,&stat_buf);
		if(S_ISREG(stat_buf.st_mode)){
			return 1;
		}
		else if(S_ISDIR(stat_buf.st_mode)){
			return 2;
		}
	}
	return -1; //파일이 존재하지 않는 경우 -1
}

ps_info* get_stat(){ //prco 폴더내에서 각 프로세스의 해당하는 stat파일을 읽어들이는 함수
	ps_info * pi = malloc(sizeof(ps_info));
	FILE * fp = fopen("stat","r");
	if(fp == NULL){
		return NULL;
	}
	fscanf(fp,"%d%s",&pi->pid,pi->name);
	getc(fp);
	fscanf(fp,"%c",&pi->state);
	pi->name[strlen(pi->name) - 1] = '\0';
	for(int i = 0 ; i < strlen(pi->name); i++){
		pi->name[i]=pi->name[i+1];
	}
	return pi;
}


void sig_handler(int signo){ //SIGUSR1이 들어온 경우의 handler
	exit(0);
}

void backup(char * fname,char * current_time){ //파일 백업 함수
	pthread_mutex_lock(&mutx); //파일 동기화를 위하여 동시에 백업 진행시 하나하나 하도록 함
	int fd_t = open(fname,MODE_A,0644);
	chdir(backup_dir_path);
	char * new_name =malloc(sizeof(char)*256);
	int count;
	strcpy(new_name,to_hex_adress(fname));
	strcat(new_name,"_");
	strcat(new_name,current_time); //16진수화_날짜 형식의 파일이름 생성
	chdir(backup_dir_path);
	int fd =open(new_name,MODE_A,0644);//백업파일 생성
	char buf[1024];
	while((count = read(fd_t,buf,1024)) > 0){ //내용 복사
		write(fd,buf,count);
	}
	lseek(fd_t,0,SEEK_SET);
	struct stat statbuf;
	stat(fname,&statbuf);
	struct tm * t;
	t = localtime(&statbuf.st_mtime); //로그 출력을위하여 mtime을 형식에 맞게 변환함
	sprintf(buf,"backup time : %s filename : %s file size : %ld file mtime : %02d%02d %02d:%02d:%02d\n",current_time,path_basename(fname),statbuf.st_size,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
	write(log_fd,buf,strlen(buf)); //로그 출력
	pthread_mutex_unlock(&mutx);
}

char* get_current_time(){ //현재시간을 형식에 맞게 문자열로 바꾼후 return 해주는 함수
	time_t timer;
	struct tm *t;
	char * result =malloc(sizeof(char) * 100);
	timer = time(NULL);
	t = localtime(&timer);
	sprintf(result,"%02d%02d%02d%02d%02d",t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
	return result;
}

char *path_basename(char* path) //경로 끝의 파일만 가죠오는 함수
{
	char *basename;

	basename = strrchr(path, '/');
	return basename ? basename +1 : path;
}

void backup_handler(int flag_n,int n_num,int flag_d,char * fname,int period){ //각 옵션에 맞게 백업을 진행하도록 하는 함수
	file_list * head = malloc(sizeof(file_list));
	struct dirent **namelist;
	struct stat sbuf;
	char * buf = malloc(sizeof(char)*1024);
	strcpy(buf,"");
	stat(fname,&sbuf);
	if(flag_d == 1){ //d옵션이 오는 경우
		int count = scandir(fname, &namelist, NULL, alphasort);
		for(int i = 0 ; i < count ; i++){ 
			if(strcmp(namelist[i]->d_name,".")==0 || strcmp(namelist[i]->d_name,"..")==0){
				continue;
			}
			if(get_filetype(address_add(fname,namelist[i]->d_name)) == 1){ //파일일 경우 쓰레드 생성 후 쓰레드를 통한 백업
				pthread_t tid;
				pthread_create(&tid, NULL, ssu_thread,(void*)address_add(fname,namelist[i]->d_name));
			}
			else if(get_filetype(address_add(fname,namelist[i]->d_name)) == 2){ //디렉톨리인경우 재귀호출을 통해 그 디렉토리 내부의 파일들 조사
				backup_handler(flag_n,n_num,1,address_add(fname,namelist[i]->d_name),period);
			}
		}		
	}
	else { //d옵션이 오지않은 경우(일반 파일의 경우)
		int count = scandir(backup_dir_path,&namelist,NULL,alphasort); //백업 디렉토리의 파일명들을 저장
		for(int i = 0 ; i < count ; i++){
			if(strcmp(namelist[i]->d_name,".") == 0 || strcmp(namelist[i]->d_name,"..")==0){
				continue;
			}
			if(strcmp(get_hex_filename(namelist[i]->d_name),to_hex_adress(fname)) == 0){ //기존의 백업된 파일들이 있는 경우 리스트에 저장(n옵션을 위해서)
				list_file_add(head,namelist[i]->d_name);
			}
		}
		time_t intertime = start_time;
		while(1){
			if(flag_m==1){ //m옵션이 오는 경우 수정될때까지 무한루프로 진행을 정지 시킴
                while(1){
            	    stat(fname,&sbuf);
            		if(intertime < sbuf.st_mtime){
            			intertime = sbuf.st_mtime;
            			break;
            		}
            		sleep(period);
            	}
			}
			stat(fname,&sbuf);
			if(intertime < sbuf.st_mtime){ //파일이 수정되는 경우 log출력
            	intertime = sbuf.st_mtime;
            	sprintf(buf,"%s modified\n",path_basename(fname));
            	write(log_fd,buf,strlen(buf));
            }
            if(access(fname,F_OK) == -1){ //파일이 삭제되는 경우 log 출력
            	sprintf(buf,"%s deleted.  process exit\n",path_basename(fname));
            	write(log_fd,buf,strlen(buf));
            	pthread_cancel(pthread_self());			
			}
			if(flag_n == 1){ //n옵션이 오는 경우
				int size = get_list_size(head); //리스트의 노드 개수 계산
				if(size < n_num){ //n옵션에 오는 인자보다 작은 경우 backup후 리스트에 추가
					list_add(head,fname);
					backup(fname,get_current_time());
				}
				else{ //아닐 경우
					while(get_list_size(head) >= n_num){ //리스트 노드 개수가 n옵션의 인자보다 작거나 같아질떄까지 파일 삭제
						file_list * current = head->next;
						struct stat statbuf;
						char temp[1024];
						strcpy(temp,backup_dir_path);
						strcat(temp,"/");
						strcat(temp,current->file_name);
						stat(temp,&statbuf);
						if(unlink(temp) < 0){
							syslog(LOG_ERR,"%s %s\n",temp,strerror(errno));
						}
						struct tm * t;
						t = localtime(&statbuf.st_mtime);
						sprintf(temp,"[%s]%s file is delete by n option. file size : %ld file btime : %02d%02d%02d%02d%02d\n",get_current_time(),path_basename(fname),statbuf.st_size,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
						write(log_fd,temp,strlen(temp));
						strcpy(temp,"");
						head -> next = current ->next;
					}
					list_add(head,fname);
					backup(fname,get_current_time());
				}
			}
			else{ //n옵션이 없는 경우 일반 백업 실행
				backup(fname,get_current_time());
			}
			sleep(period);
		}
	}
}

int get_list_size(file_list * head){ //list의 노드 개수를 return
	int result=0;
	file_list * current;
	if(head -> next == NULL){
		return result;
	}
	else{
		current = head->next;
		while(current != NULL){
			result++;
			current = current ->next;
		}
		return result;
	}

}
void list_file_add(file_list * head , char* file_name){ //리스트의 끝에 노드를 추가해주는 함수
	file_list * node = malloc(sizeof(file_list));
	node -> next = NULL;
	if(head -> next == NULL){
		strcpy(node -> file_name,file_name);
		head -> next = node;
	}
	else{
		file_list * current = head->next;
		while(current->next != NULL){
			current = current ->next;
		}
		strcpy(node -> file_name,file_name);
		current -> next = node;
	}
}

void list_add(file_list * head,char* fname){ //리스트의 끝에 노드를 추가해주는 함수
	char * new_name =malloc(sizeof(char)*256);
	strcpy(new_name,to_hex_adress(fname));
	strcat(new_name,"_");
	strcat(new_name,get_current_time());
	file_list * node = malloc(sizeof(file_list));
	node -> next = NULL;
	if(head -> next == NULL){
		strcpy(node -> file_name,new_name);
		head -> next = node;
	}
	else{
		file_list * current = head->next;
		while(current->next != NULL){
			current = current ->next;
		}
		strcpy(node -> file_name, new_name);
		current -> next = node;
	}
	
}

void list_delete(file_list * head){ //더미노드 뒤에 오는 노드를 삭제해주는 함수
	file_list * temp;
	if(head->next == NULL){
		return;
	}
	temp = head -> next;
	head -> next = head->next->next;
	free(temp);
}

void * ssu_thread(void* arg){ //thread 함수
	char fname[256];
	strcpy(fname,(char*)arg);
	backup_handler(flag_n,n_num,0,fname,period);
}

char* address_add(char * s1, char* s2){ //경로 두개를 합쳐주는 함수
	char * result = malloc(sizeof(char)*1024);
	strcpy(result,"");
	strcat(result,s1);
	if(s1[strlen(s1)-1] != '/'){
		strcat(result,"/");
	}
	strcat(result,s2);
	return result;
}
void restore(){ //r옵션을 위한 함수
	int count;
	int c;
	file_list * head = malloc(sizeof(file_list));
	struct dirent ** namelist;
	char buf[1024];
	strcpy(buf,"");
	count = scandir(backup_dir_path,&namelist,NULL,alphasort); //백업 디렉토리에서 파일들을 읽어들임
	struct stat stat_buf;
	for(int i = 0 ; i < count ; i++){
		if(strcmp(namelist[i]->d_name,".") == 0 || strcmp(namelist[i]->d_name,"..") == 0){
			continue;
		}
		if(strcmp(get_hex_filename(namelist[i]->d_name),to_hex_adress(target_path)) == 0){ //같은 이름을 가진 백업파일들 리스트 생성
			list_file_add(head,namelist[i]->d_name);
		}
	}
	if(get_list_size(head) == 0){
		fprintf(stderr,"backup file not exist\n.");
		exit(1);
	}
	printf("[0] exit\n");
	for(int i = 0 ; i < get_list_size(head) ; i++){ //복구를 위한 리스트 목록 출력, 
		stat(address_add(backup_dir_path,get_node(i,head)->file_name),&stat_buf);
		printf("[%d] %s [size %ld]\n",i+1, path_basename(hex_to_origin(get_node(i,head)->file_name)),stat_buf.st_size);
	}
	scanf("%d",&c);
	if(c == 0){
		exit(0);
	}
	else{
		printf("Recovery backup file...\n");
		printf("[%s]\n",path_basename(target_path));
		int fd_t = open(target_path,MODE_W);
		int fd_s = open(address_add(backup_dir_path,get_node(c-1,head)->file_name),MODE_R);
		while((count = read(fd_s,buf,1024)) > 0){ //백업파일을 원복파일로 복사
			write(fd_t,buf,count);
		}
	}
}

void compare(){ //c옵션을 위한 함수
	char * file_name = malloc(sizeof(char)*200);
	strcpy(file_name,"");
	struct dirent ** namelist;
	int count = scandir(backup_dir_path,&namelist,NULL,alphasort); //백업 디렉토리에서 파일들을 읽어들임
	for(int i = 0 ; i < count ; i++){
		if(strcmp(namelist[i]->d_name,".") == 0 || strcmp(namelist[i]->d_name,"..") == 0){
			continue;
		}
		if(strcmp(get_hex_filename(namelist[i]->d_name),get_hex_filename(to_hex_adress(target_path))) == 0){ //같은 이름을 가진 백업파일의 제일 최신 파일을 저장
			strcpy(file_name,namelist[i]->d_name);
		}
	}
	if(strcmp(file_name,"") == 0){
		fprintf(stderr,"backup file not exist\n.");
		exit(1);
	}
	printf("<Compare with backup file[%s]>\n",hex_to_origin(file_name));
	pid_t pid = fork();
	if(pid == 0){
		execlp("diff","diff",target_path,address_add(backup_dir_path,file_name),NULL); //diff명령어 실행
	}
	else if (pid > 0){
		wait(NULL);
	}
}

file_list* get_node(int index,file_list * head){ //인자로 주어진 인덱스의 맞는 리스트의 노드 주소값 return
	file_list * current = head;
	for(int i = 0 ; i <= index; i++){
		current = current->next;
	}
	return current;
}

char* getdir(char * path){ //주어진 경로의 디렉토리를 구하는 함수(디렉토리일경우 바로 return 파일일 경우 그 파일이 속한 디렉토리 리턴)
	char * result = malloc(sizeof(char) * 300);
	char * temp = malloc(sizeof(char) * 300);
	strcpy(temp,path);
	if(get_filetype(temp) == 1){
		strncpy(result,temp,strlen(temp) - strlen(path_basename(temp)));
		result[strlen(temp) - strlen(path_basename(temp))] = '\0';
	}
	else{
		strcpy(result,path);
	}
	return result;
}

char* get_hex_filename(char * hex_name){ //주어진 백업파일에서 _날짜 부분을 뺀 파일이름을 return 해주는 함수
	char * temp = malloc(sizeof(char)*300);
	char * result = malloc(sizeof(char)*300);
	strcpy(temp,hex_name);
	strcpy(result,"");
	for(int i = 0; i < strlen(temp) ; i++){
		if(temp[i]=='_'){
			result[i] = '\0';
			break;
		}
		else{
			result[i]=temp[i];
		}
	}
	return result;
}

int get_dir_file_count(char * path){ //폴더내의 일반파일의 개수 파악
	struct dirent **namelist;
	int result = 0;
	char * fname = malloc(sizeof(char)*300);
	strcpy(fname,path);
	int count = scandir(fname,&namelist,NULL,alphasort);
	for(int i = 0 ; i < count ; i++){
		if(strcmp(namelist[i]->d_name,".") == 0 || strcmp(namelist[i]->d_name,"..") == 0|| strstr(namelist[i]->d_name,".swp") != NULL){
			continue;
		}
		if(get_filetype(address_add(fname,namelist[i]->d_name)) == 1){ //일반파일일경우 개수 증가
			result++;
		}
		else if(get_filetype(address_add(fname,namelist[i]->d_name)) == 2){ //디렉토리일경우 재귀
			result = result + get_dir_file_count(address_add(fname,namelist[i]->d_name));
		}
	}
	return result;
}

char* get_latest_file(char * fname){ //디렉토리내에 가장 최근에 수정된 파일의 파일명 return
	struct dirent ** namelist;
	char * result = malloc(sizeof(char)*200);
	strcpy(result,"");
	time_t mtime = 0;
	struct stat stat_buf;
	int count = scandir(fname,&namelist,NULL,alphasort);
	for(int i = 0 ; i < count ; i++){
		if(strcmp(namelist[i]->d_name,".") == 0 || strcmp(namelist[i]->d_name,"..") == 0){
			continue;
		}
		if(get_filetype(address_add(fname,namelist[i]->d_name)) == 1){ //일반파일일경우 지금까지 검사한 파일중 가장 최신인 파일과 비교
			stat(address_add(fname,namelist[i]->d_name),&stat_buf);
			if(stat_buf.st_mtime > mtime){
				mtime = stat_buf.st_mtime;
				strcpy(result,address_add(fname,namelist[i]->d_name));
			}
		}
		else if(get_filetype(address_add(fname,namelist[i]->d_name)) == 2){ //디렉토리인경우 재귀호출을 통해 디렉토리내에서 가장 최신인 파일을 가지고 와서 비교
			stat(get_latest_file(address_add(fname,namelist[i]->d_name)),&stat_buf);
			if(stat_buf.st_mtime > mtime){
				mtime = stat_buf.st_mtime;
				strcpy(result,get_latest_file(address_add(fname,namelist[i]->d_name)));
			}
		}
	}
	return result;
}
