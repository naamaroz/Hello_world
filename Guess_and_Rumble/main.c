#include <sys/time.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "sched.h"
#include <stdio.h>
#include <netinet/in.h>

#include <signal.h>
//#include <gnutls/gnutls.h>
//#include <nettle/nettle-types.h>


//For TCP server
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>


#include "EvictionSet.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define DEMO 0

LinesBuffer Buffer;

Cache_Mapping EvictionSets;
Cache_Statistics CacheStats;
Cache_Statistics OrderdStats;

Cache_Statistics FirstStatistics;
Cache_Statistics SecondStatistics;
Cache_Statistics DiffStatistics1, DiffStatistics2;
void* close_while_noise();
int exitflag; //exit flag when in the noise loop
extern FILE *pFile;

#define NET_CONTROL 1

int open_socket();
//For Tcp Server
int socket_desc , client_sock , c , read_size;
struct sockaddr_in server , client;
char client_message[1];

int PORT=7000;
char* RsaKeys[1];
void sighandler(int sig);

int printMenu();


int main(int argc,char* argv[])
{
	//	char Input[100];
	//	int SetsToMonitor[SETS_IN_CACHE][2];
	//	int SetsToMonitorNum;
	//	int Set;
	int i;
	//	int ByteInKey;
	int input;
	int SuspectedSets[SETS_IN_CACHE];
	int numOfSuspects;
	if(argc==2){
		printf("%s\n",argv[1]);
		PORT=atoi(argv[1]);
	}
	exitflag = 0;
	memset(&Buffer, 0, sizeof(Buffer));
	signal(SIGINT, sighandler);

	printf("---------------------------------- Start mapping the cache ----------------------------------\n");
	if (DEMO){
		printf("Press any key.\r\n");
		scanf("%*c");
	}
	#ifdef NET_CONTROL
	open_socket();
	#endif
	
	int SetsFound = CreateEvictionSetFromBuff(&Buffer, &EvictionSets);
	
	#ifdef NET_CONTROL
	char sets_found_string[6];
	sprintf(sets_found_string,"%d\n",SetsFound);	
	write(client_sock, sets_found_string,strlen(sets_found_string));
	#else
	printf("\nTotal Sets found: %d\r\n", SetsFound);
	#endif
	
	while(1){

		//Receive a message from client
		#ifdef NET_CONTROL
		read_size = recv(client_sock , client_message , 1 , 0);
		printf("%c\n",*client_message);
		if(read_size == 0)
		{
			puts("Client disconnected");
			fflush(stdout);
			close(socket_desc);
			exit(0);
		}
		else if(read_size == -1)
		{
			perror("recv failed");
		}

		if (*client_message == '1')
		{
			GetMemoryStatistics(&EvictionSets, &CacheStats);
			SortStatistics(&CacheStats,&OrderdStats);
			for(i = 0 ; OrderdStats.SetStats[SetsFound - i -1].num > 1100 ; i++){
				unsigned int SetNum = (unsigned int)OrderdStats.SetStats[SetsFound - i -1].mean;
				printf("%d) Set %d, Num %ld, mean %lf, var %lf, offset %lx\r\n",i, SetNum, (long int)CacheStats.SetStats[SetNum].num,
						CacheStats.SetStats[SetNum].mean, CacheStats.SetStats[SetNum].variance, CacheStats.SetStats[SetNum].offset);
			}
		}
		else if(*client_message == '4'){
			close(client_sock);
			close(socket_desc);
			exit(0);
		}
		else if(*client_message == '2'){
			numOfSuspects = findsuspectsWithPatterns(&EvictionSets,SuspectedSets,SetsFound);
			char client_msg[20];
			sprintf(client_msg,"%d\n",numOfSuspects);
			write(client_sock,client_msg,strlen(client_msg));
		}
		else if(*client_message == '3'){
			pthread_t close_while_noise_th;
			if(pthread_create(&close_while_noise_th, NULL, close_while_noise,0)) {
			fprintf(stderr, "Error creating thread\n");
			return 1;

			}
			printf("Making noise\nctrl + c to stop\n");
			char client_msg[20];
			sprintf(client_msg,"Making noise\n");
			write(client_sock,client_msg,strlen(client_msg));
			noiseSuspectedSets(&EvictionSets,SuspectedSets,numOfSuspects,1);
			if(pthread_join(close_while_noise_th, NULL)) {

			fprintf(stderr, "Error joining thread\n");
			return 2;

			}
		}
		else
			continue;
		#else
		input = printMenu();
		if(input == 1)
		{
			GetMemoryStatistics(&EvictionSets, &CacheStats);
			SortStatistics(&CacheStats,&OrderdStats);
			for(i = 0 ; OrderdStats.SetStats[SetsFound - i -1].num > 1100 ; i++){
				unsigned int SetNum = (unsigned int)OrderdStats.SetStats[SetsFound - i -1].mean;
				printf("%d) Set %d, Num %ld, mean %lf, var %lf, offset %lx\r\n",i, SetNum, (long int)CacheStats.SetStats[SetNum].num,
						CacheStats.SetStats[SetNum].mean, CacheStats.SetStats[SetNum].variance, CacheStats.SetStats[SetNum].offset);
			}	
		}
		else if(input == 2)
		{
			numOfSuspects = findsuspectsWithPatterns(&EvictionSets,SuspectedSets,SetsFound);
			printf("number of high activity sets: %d\n",numOfSuspects);
		}
		else if(input == 3)
		{
			pthread_t close_while_noise_th;
			if(pthread_create(&close_while_noise_th, NULL, close_while_noise,0)) {
			fprintf(stderr, "Error creating thread\n");
			return 1;
			}
			printf("Making noise\nctrl + c to stop\n");
			char client_msg[20];
			sprintf(client_msg,"Making noise\n");
			write(client_sock,client_msg,strlen(client_msg));
			noiseSuspectedSets(&EvictionSets,SuspectedSets,numOfSuspects,1);
//			noiseRandLine(&EvictionSets,SuspectedSets,numOfSuspects);
//			noiseRandSets(&EvictionSets,SuspectedSets,numOfSuspects,20);
			if(pthread_join(close_while_noise_th, NULL)) {

			fprintf(stderr, "Error joining thread\n");
			return 2;
			}
		}
		else if(input == 4)
			exit(0);
		#endif
	}
	GetMemoryStatistics(&EvictionSets, &CacheStats);

	WriteStatisticsToFile(&CacheStats, "statistics.txt", 1);
	return 0;
}


void sighandler(int sig){
	signal(sig,SIG_IGN);
	if(exitflag == 1){
		exitflag = 0;
		//fclose(pFile);
		signal(SIGINT,sighandler);
	}
	else{
		#ifdef NET_CONTROL
		close(client_sock);
		close(socket_desc);
		#endif		
		exit(0);
	}
}

int open_socket(){
	//Create socket
	socket_desc = socket(AF_INET , SOCK_STREAM , 0);
	if (socket_desc == -1)
	{
		printf("Could not create socket");
	}
	puts("Socket created");

	//Prepare the sockaddr_in structure
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons( PORT );

	//Bind
	if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
	{
		//print the error message
		perror("bind failed. Error");
		exit(1);
	}
	puts("bind done");

	//Listen
	listen(socket_desc , 1);
	puts("Waiting for incoming connections...");
	c = sizeof(struct sockaddr_in);

	//accept connection from an incoming client
	client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c);
	if (client_sock < 0)
	{
		perror("accept failed");
		exit(1);
	}
	puts("Connection accepted");
	return 0;
}

void* close_while_noise(){
	int read_size = recv(client_sock , client_message , 1 , 0);
	write(client_sock , client_message , strlen(client_message));
	printf("%c\n",*client_message);
	if(read_size == 0)
	{
		puts("Client disconnected");
		fflush(stdout);
		close(socket_desc);
		exit(0);
	}
	else if(read_size == -1)
	{
		perror("recv failed");
	}
	exitflag =0;
	return NULL;
}

int printMenu()
{
	int input_int;
	printf("--------------------------------------------Menu---------------------------------------------\n");
	printf("1)Get stats\n2)Find suspicious sets\n3)Rumble suspects\n4)Exit\n");
	printf("Please enter your choice\n");
	scanf("%d",&input_int);
	fflush(stdin);
	return input_int;
}
