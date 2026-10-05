#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9410


/*
 * Function executed by each Controller thread
 */
void *handle_client(void *arg)
{
    int connfd = *(int *)arg;

    /*
     * Print message when thread starts
     */
    printf("Controller is being handled by a thread.\n\n");

    fflush(stdout);

    /*
     * Keep the connection open
     * so we can see multiple Controllers.
     *
     * In a real RemoteOps Agent,
     * commands will be handled here.
     */
    sleep(10);

    /*
     * Close Controller connection
     */
    close(connfd);

    /*
     * Free allocated memory
     */
    free(arg);

    return NULL;
}


int main()
{
    int listenfd;
    int *connfd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len;

    pthread_t thread;


    /*
     * Create TCP socket
     */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    if (listenfd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }


    /*
     * Allow reuse of port
     */
    int opt = 1;

    if (setsockopt(listenfd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) < 0)
    {
        perror("setsockopt");

        close(listenfd);

        exit(EXIT_FAILURE);
    }


    /*
     * Clear server address structure
     */
    memset(&server_addr,
           0,
           sizeof(server_addr));


    /*
     * IPv4
     */
    server_addr.sin_family = AF_INET;


    /*
     * Accept connections
     * from any network interface
     */
    server_addr.sin_addr.s_addr = INADDR_ANY;


    /*
     * Port 9410
     */
    server_addr.sin_port = htons(PORT);


    /*
     * Bind socket
     */
    if (bind(listenfd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");

        close(listenfd);

        exit(EXIT_FAILURE);
    }


    /*
     * Start listening
     */
    if (listen(listenfd, 10) < 0)
    {
        perror("listen");

        close(listenfd);

        exit(EXIT_FAILURE);
    }


    /*
     * Required output
     */
    printf("RemoteOps Agent listening on port %d...\n\n",
           PORT);

    fflush(stdout);


    /*
     * Continuously accept Controllers
     */
    while (1)
    {
        client_len = sizeof(client_addr);


        /*
         * Accept Controller
         */
        int new_connfd = accept(
            listenfd,
            (struct sockaddr *)&client_addr,
            &client_len
        );


        if (new_connfd < 0)
        {
            perror("accept");

            continue;
        }


        /*
         * Allocate memory for
         * Controller socket
         */
        connfd = malloc(sizeof(int));

        if (connfd == NULL)
        {
            perror("malloc");

            close(new_connfd);

            continue;
        }


        /*
         * Store connection socket
         */
        *connfd = new_connfd;


        /*
         * Required output
         */
        printf("Controller connected successfully!\n");

        fflush(stdout);


        /*
         * Create a thread
         */
        if (pthread_create(
                &thread,
                NULL,
                handle_client,
                connfd
            ) != 0)
        {
            perror("pthread_create");

            close(new_connfd);

            free(connfd);

            continue;
        }


        /*
         * Detach thread
         */
        pthread_detach(thread);
    }


    /*
     * Close listening socket
     */
    close(listenfd);

    return 0;
}


  
