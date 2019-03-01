#include "threads.h"
#include <sys/socket.h>
#include <stdlib.h>
#include <semaphore.h>
#include <stdio.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <setjmp.h>

#define PORTNUM 9001

typedef struct socketStruct
{
    int             socketFd;
    struct sockaddr *address;
    int             sockaddrlen;
} socketStruct;

void parseHTML(uint64_t job);
char *getBanner(uint64_t type, uint64_t size, char *fLoc, uint64_t *fileType);
//char *buildRequest(char *filePath);
char *buildRequest(char *filePath, uint64_t *type, int64_t *fd);

uint8_t RUNNING = 1;
jmp_buf sigExit;

void
ignoreSIGINT(
    __attribute__ ((unused))
    int sig_num)
{
	RUNNING = 0;
	longjmp(sigExit, 0);
}

char *siteDir = NULL;

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		fprintf(stderr, "Requires directory.\n");
		return 1;
	}
    struct sigaction ignore = {
        .sa_handler = &ignoreSIGINT,
        .sa_flags = SA_RESTART
    };
    sigaction(SIGINT, &ignore, NULL);
	char *filePath = argv[1];
	siteDir = calloc(strlen(filePath) + 6, sizeof(*siteDir));
	strcat(siteDir, filePath);
	//strcat(siteDir, "/www/");
	t_pool *myPool = init_t_pool(8);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
	if (setsockopt
		(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
		 sizeof(opt)) != 0)
	{
		perror("Unable to set socket options.\n");
		exit(1);
	}
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(PORTNUM)
	};
	socketStruct    mySock = {
		.socketFd = fd,
		.address = (struct sockaddr *) &address,
		.sockaddrlen = sizeof(address)
	};
	if (bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0)
	{
		perror("Unable to bind socket.\n");
		exit(1);
	}
    listen(mySock.socketFd, 2);
	int socketNum = 0;
	setjmp(sigExit);
	while(RUNNING)
	{
		socketNum = accept(mySock.socketFd, 
				mySock.address, 
				(socklen_t *)&mySock.sockaddrlen);
		add_job(myPool, socketNum);
	}
	free(siteDir);
	reap_t_pool(myPool, 8);
	destroy_t_pool(myPool);
	return 0;
}

void parseHTML(uint64_t job)
{
	uint64_t size = 256;
	char *buff = calloc(size, sizeof(*buff)+1);
	read(job, buff, size);
	uint64_t tokenId = 0;
	char *savePtr = NULL;
	char *token = strtok_r(buff, " ", &savePtr);
	char *reply = NULL;
	uint64_t fileType = 0;
	int64_t fileDesc = 0;
	char *cgiCmd = NULL;
	while(token != NULL)
	{
		switch(tokenId)
		{
			case(0):
			{
				if(strcmp(token, "GET") != 0)
				{
					char *error = getBanner(1, 0, NULL, NULL);
					write(job, error, strlen(error));
					free(error);
					free(buff);
					return;
				}
				break;
			}
			case(1):
			{
				reply = buildRequest(token, &fileType, &fileDesc);
				if(reply == NULL)
				{
					reply = getBanner(2, 0, NULL, NULL);
				}
				if(fileType == 2)
				{
					cgiCmd = reply;
				}
				break;
			}
			case(2):
			{
				/* Check HTTP version */
				if(strcmp(token, "HTTP/1.1") != 0)
				{
					fprintf(stderr, "Bad HTTP VERSION: %s\n", token);
					char *error = getBanner(1, 0, NULL, NULL);
					write(job, error, strlen(error));
					free(buff);
					return;
				}
				break;
			}
			case(3):
			{
				/* Verify Host */
				if(strcmp(token, "Host:") != 0)
				{
					fprintf(stderr, "NO HOST\n");
					char *error = getBanner(1, 0, NULL, NULL);
					write(job, error, strlen(error));
					free(reply);
					free(buff);
					free(error);
					return;
				}
				break;
			}

		}
		token = strtok_r(NULL, " \r\n", &savePtr);
		tokenId++;
	}
	if(fileDesc == -1)
	{
		char *error = getBanner(2, 0, NULL, NULL);
		write(job, error, strlen(error));
		free(buff);
		free(reply);
		free(error);
		return;

	}
	printf("Test: %lu\n", fileType);
	if(fileType == 0)
	{
		uint64_t fSize = lseek(fileDesc, 0, SEEK_END);
		lseek(fileDesc, 0, SEEK_SET);
		char *body = calloc(1, fSize + 1);
		uint64_t byteRead = read(fileDesc, body, fSize);
		if(byteRead != fSize)
		{
			/* 404 */
			char *error = getBanner(2, 0, NULL, NULL);
			write(job, error, strlen(error));
			free(buff);
			free(reply);
			return;
		}
		reply = realloc(reply, strlen(body) + strlen(reply) + 1);
		strcat(reply, body);
		free(body);
		write(job, reply, strlen(reply));
	}
	else if(fileType == 1)
	{
		uint64_t fSize = lseek(fileDesc, 0, SEEK_END);
		lseek(fileDesc, 0, SEEK_SET);
		write(job, reply, strlen(reply));
		sendfile(job, fileDesc, NULL, fSize);
	}
	else if(fileType == 2)
	{
		char *cgiQuery = strchr(reply, '?');
		if(cgiQuery != NULL)
		{
			*cgiQuery = '\0';
			cgiQuery++;
			setenv("QUERY_STRING", cgiQuery, 1);
		}	
		FILE *script = popen(cgiCmd, "r");
		char *results = NULL;
		char *fileBuff = NULL;
		if(script != NULL)
		{
			results = calloc(257, sizeof(*results));
			fileBuff = calloc(257, sizeof(*fileBuff));
			if(results == NULL || fileBuff == NULL)
			{
				fprintf(stderr, "Cgi buffs calloc failed\n");
				exit(10);
			}
			int numRead = 0;
			while((numRead = fread(fileBuff, 1, 256, script)) == 256)
			{
				results = realloc(results, strlen(results) + numRead + 2);
				strncat(results, fileBuff, numRead);
			}
			results = realloc(results, strlen(results) + numRead + 2);
			strncat(results, fileBuff, numRead);
		}
		if(pclose(script) != 0)
		{
			fprintf(stderr, "ERROR 500\n");
			reply = getBanner(3, 0, NULL, NULL);
			write(job, reply, strlen(reply));
			free(buff);
			free(fileBuff);
			free(results);
			free(cgiCmd);
			free(reply);
			return;
		}
		char *banner = getBanner(0, strlen(results), reply, NULL);
		write(job, banner, strlen(banner));
		write(job, results, strlen(results));
		free(fileBuff);
		free(results);
		free(banner);
	}
	free(reply);
	free(buff);
	return;
}

char *buildRequest(char *filePath, uint64_t *type, int64_t *fd)
{
	char basePath[] = "%s%s%s";
	char *retFilePath = NULL;
	if(strcmp(filePath, "/") == 0)
	{
		asprintf(&retFilePath, basePath, siteDir, "/www/", "index.html");	
		*fd = open(retFilePath, O_RDONLY);
		if(*fd == -1)
		{
			free(retFilePath);
			return NULL;
		}
		uint64_t fSize = lseek(*fd, 0, SEEK_END);
		lseek(*fd, 0, SEEK_SET);
		char *banner = getBanner(0, fSize, retFilePath, type);
		free(retFilePath);
		return banner;
	}
	else if(strncmp(filePath, "/cgi-bin/", 9) == 0)
	{
		char *file = strrchr(filePath, '/');
		asprintf(&retFilePath, basePath, siteDir, "/cgi-bin", file++);	
		*type = 2;
		/* Open file and return */
		return retFilePath;
	}
	else
	{
		asprintf(&retFilePath, basePath, siteDir, "/www", filePath++);	
		*fd = open(retFilePath, O_RDONLY);
		if(*fd == -1)
		{
			free(retFilePath);
			return NULL;
		}
		uint64_t fSize = lseek(*fd, 0, SEEK_END);
		lseek(*fd, 0, SEEK_SET);
		char *banner = getBanner(0, fSize, retFilePath, type);
		free(retFilePath);
		return banner;
	}
}

char *getBanner(uint64_t type, uint64_t size, char *fLoc, uint64_t *fileType)
{
	char httpBanner[] = "HTTP/1.1 %d %s\r\n"
						"Content-Type:%s\r\n"
						"Content-Length:%d\r\n\r\n";

	char cgiBanner[] = "HTTP/1.1 %d %s\r\n"
					   "Content-Length:%d\r\n";
	
	char *retString = NULL;
	switch(type)
	{
		case(0): 	/* OK */
			{
				char *fExt = strrchr(fLoc, '.');
				if(strcmp(fExt, ".css") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "text/css", size);
					*fileType = 0;
				}
				else if(strcmp(fExt, ".txt") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "text/plain", size);
					*fileType = 0;
				}
				else if(strcmp(fExt, ".jpeg") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "image/jpeg", size);
					*fileType = 1;
				}
				else if(strcmp(fExt, ".png") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "image/png", size);
					*fileType = 1;
				}
				else if(strcmp(fExt, ".gif") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "image/gif", size);
					*fileType = 1;
				}
				else if(strcmp(fExt, ".html") == 0)
				{
					asprintf(&retString, httpBanner, 200, "OK", "text/html", size);
					*fileType = 0;
				}
				else
				{
					asprintf(&retString, cgiBanner, 200, "OK", size);
				}
				break;
			}
		case(1):	/* 400 */
			{
				char err400[] = "<!doctype html>\n"
					"<html lang=\"en\">\n"
					"<head>\n"
					"<title>Invalid Request</title>\n"
					"</head>\n"
					"<body>\n"
					"<h2>Error 400</h2>\n"
					"</body>\n"
					"</html>\n";
				asprintf(&retString, httpBanner, 400, "Bad Request", "text/html", strlen(err400));
				retString = realloc(retString, strlen(retString) + strlen(err400) + 1);
				strcat(retString, err400);
				break;
			}
		case(2):	/* 404 */
			{
				char err404[] = "<!doctype html>\n"
					"<html lang=\"en\">\n"
					"<head>\n"
					"<title>Page Not Found</title>\n"
					"</head>\n"
					"<body>\n"
					"<h2>Error 404</h2>\n"
					"</body>\n"
					"</html>\n";
				asprintf(&retString, httpBanner, 404, "Not Found", "text/html", strlen(err404));
				retString = realloc(retString, strlen(retString) + strlen(err404) + 1);
				strcat(retString, err404);
				break;
			}
		case(3):	/* 500 */
			{
				char err500[] = "<!doctype html>\n"
					"<html lang=\"en\">\n"
					"<head>\n"
					"<title>Internal Server Error</title>\n"
					"</head>\n"
					"<body>\n"
					"<h2>Error 500</h2>\n"
					"</body>\n"
					"</html>\n";
				asprintf(&retString, httpBanner, 500, "Internal Server Error", "text/html", strlen(err500));
				retString = realloc(retString, strlen(retString) + strlen(err500) + 1);
				strcat(retString, err500);
				break;
			}
			break;
	}
	return retString;
}
