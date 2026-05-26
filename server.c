// posix va definito prima di tutto
#define _POSIX_C_SOURCE 200809L // per avere accesso a funzioni come localtime_r e strftime

#include <arpa/inet.h>  /* funzioni per indirizzi IP e byte order */
#include <netinet/in.h> /* strutture per indirizzi IPv4 */
#include <pthread.h>    /* pthread_create, pthread_detach, mutex */
#include <stdio.h>      /* printf, perror */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, bind, listen, accept, recv */
#include <time.h>       /* time, localtime_r, strftime */
#include <unistd.h>     /* close */
#include <signal.h>     /* libreria di sigint*/
#include <errno.h>      /* errno per error handling */

#define SERVER_PORT 8080
#define BACKLOG 5        // numero massimo di connessioni in attesa
#define BUFFER_SIZE 1024 // dimensione massima del messaggio ricevuto
#define LOG_FILE_NAME "server.log"
#define MAX_CLIENTS 5         // numero massimo di client che il server può gestire contemporaneamente
#define LOG_CHECK_INTERVAL 10 // intervallo in secondi per il controllo del file di log
#define LOG_SIZE_LIMIT 1024   // dimensione massima del file di log in byte

FILE *log_file;                                        // puntatore al file di log condiviso tra i thread che gestiscono i client
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; // lucchetto per l'accesso singolo al file di log tra i thread

// variabile per lo stop del programma (ctrl + c)
volatile __sig_atomic_t server_running = 1;
// variabile log_check
volatile sig_atomic_t log_check = 0;

int active_clients = 0;                                   // variabile per tenere traccia dei client attivi
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER; // lucchetto per l'accesso alla variabile active_clients
pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;    // variabile di condizione per notificare il thread principale quando un client si disconnette

void remove_newline(char *text)
{
    int text_clean = strcspn(text, "\r\n"); // trova la posizione del primo \n o \r
    text[text_clean] = '\0';                // termina la stringa al primo \n o \r, rimuovendo il newline finale
}

void get_timestamp(char *timestamp, size_t size)
{
    time_t now = time(NULL); /* tempo corrente */
    struct tm local_time;    /* data locale */

    localtime_r(&now, &local_time);                              /* converte in data leggibile */
    strftime(timestamp, size, "%Y-%m-%d %H:%M:%S", &local_time); /* formatta timestamp */
}

void write_log_message(const char *message)
{
    char copy[BUFFER_SIZE];  /* copia modificabile */
    char timestamp[32];      /* timestamp formattato */
    char *separator;         /* virgola tra ID e dato */
    const char *producer_id; /* ID produttore */
    const char *data;        /* dato ricevuto */

    snprintf(copy, sizeof(copy), "%s", message); // copio il messaggio dato che poi vado a modificarlo
    remove_newline(copy);

    separator = strchr(copy, ','); /* cerca separatore ID,dato */
    if (separator != NULL)
    {
        *separator = '\0';    /* separa ID e dato */
        producer_id = copy;   /* parte prima della virgola */
        data = separator + 1; /* parte dopo la virgola */
    }
    else
    {
        producer_id = "UNKNOWN"; /* ID mancante */
        data = copy;             /* messaggio senza virgola */
    }

    get_timestamp(timestamp, sizeof(timestamp)); /* crea timestamp */

    pthread_mutex_lock(&log_mutex);                                    /* prende lock file */
    fprintf(log_file, "[%s, %s, %s]\n", timestamp, producer_id, data); /* scrive log */
    fflush(log_file);                                                  /* forza scrittura nel file log */
    pthread_mutex_unlock(&log_mutex);                                  /* rilascia lock file */
}

void write_logout_message(const char *producer_id)
{
    char timestamp[32]; /* timestamp formattato */

    get_timestamp(timestamp, sizeof(timestamp));                         /* crea timestamp */
    pthread_mutex_lock(&log_mutex);                                      /* prende lock file */
    fprintf(log_file, "[%s, %s, DISCONNECT]\n", timestamp, producer_id); /* scrive log di logout */
    fflush(log_file);                                                    /* forza scrittura nel file log */
    pthread_mutex_unlock(&log_mutex);                                    /* rilascia lock file */
}

void handle_sigint(int sigint)
{

    (void)sigint; // ignora parametro non usato
    server_running = 0;
}

void handle_sigalrm(int sigalrm)
{
    (void)sigalrm;             // ignora parametro non usato
    log_check = 1;             // setta flag per controllo log
    alarm(LOG_CHECK_INTERVAL); // reimposta allarme per il prossimo controllo del log dopo LOG_CHECK_INTERVAL secondi
}

void handle_sigpipe(int sigpipe)
{
    (void)sigpipe; // ignora parametro non usato
}

void new_log()
{
    pthread_mutex_lock(&log_mutex); // prendi lock per accedere al file di log

    fseek(log_file, 0, SEEK_END);    // sposta il puntatore alla fine del file
    long log_size = ftell(log_file); // ottieni la dimensione del file di

    if (log_size >= LOG_SIZE_LIMIT)
    {                     // se il file di log supera la dimensione limite
        fclose(log_file); // chiudi il file di log corrente

        // rinomina il file di log corrente con un timestamp
        char timestamp[32];
        get_timestamp(timestamp, sizeof(timestamp));
        char new_log_name[64];
        snprintf(new_log_name, sizeof(new_log_name), "log_%s.log", timestamp);
        if (rename(LOG_FILE_NAME, new_log_name) == -1)
        {
            perror("rename");                 // errore rinomina file
            pthread_mutex_unlock(&log_mutex); // rilascia lock
            return;
        }

        // crea un nuovo file di log
        log_file = fopen(LOG_FILE_NAME, "a");
        if (log_file == NULL)
        {
            perror("fopen");                  // errore apertura nuovo file di log
            pthread_mutex_unlock(&log_mutex); // rilascia lock
            return;
        }

        printf("File di log ruotato: %s\n", new_log_name);
    }

    pthread_mutex_unlock(&log_mutex); // rilascia lock
}

void *handle_client(void *arg) // qui arg è generico, quindi dobbiamo fare il cast del client_fd da void* a int
{
    int client_fd = *(int *)arg; // puntatore del socket del client passato come argomento, lo dereferenzio
                                 //(ovvero accedo al valore dove è punato in memoria) per ottenere il valore intero del socket
    free(arg);                   // libera la memoria allocata per il socket del client
                                 // dato che il thread non ha piu bisogno della memoria dinamica dato lo abbiamo copiato in client_fd

    char buffer[BUFFER_SIZE]; // dati ricevuti dai client
    ssize_t bytes_read;       // numero di byte letti  ( > 0 (dati ricevuti), 0 (client chiuso), -1 (errore))

    char producer_id[BUFFER_SIZE] = "UNKNOWN"; // ID del produttore

    bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); /* riceve dati */

    // rimaniamo in ascolto finché il client non chiude la connessione (bytes_read == 0)
    while (bytes_read > 0)
    {
        buffer[bytes_read] = '\0'; /* aggiunge terminatore di stringa al messaggio ricevuto */

        char *separator = strchr(buffer, ','); /* cerca separatore ID,dato */
        if (separator != NULL)
        {
            *separator = '\0';                                        /* separa ID e dato */
            snprintf(producer_id, sizeof(producer_id), "%s", buffer); /* salva ID produttore per il log di disconnessione */
            *separator = ',';                                         /* ripristina il messaggio originale con ID,DATO */
        }

        printf("Messaggio ricevuto: %s\n", buffer);
        write_log_message(buffer);                                /* scrive messaggio nel log */
        bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); /* riceve dati */
    }

    if (bytes_read == -1)
    {
        perror("recv"); /* errore recv */
    }
    else
    {
        printf("%s ha chiuso la connessione.\n", producer_id); /* client ha chiuso la connessione */
        write_logout_message(producer_id);                     /* scrive messaggio di chiusura nel log */
    }

    close(client_fd);

    pthread_mutex_lock(&thread_mutex);
    active_clients--;

    if (active_clients == 0)
    {
        pthread_cond_signal(&thread_cond);
    }

    pthread_mutex_unlock(&thread_mutex);

    return NULL; // termina il thread
}

int main(void)
{
    int server_fd;                  /* socket di ascolto che usa bind, listen e accept */
    struct sockaddr_in server_addr; /* indirizzo del server */

    log_file = fopen(LOG_FILE_NAME, "a"); /* apre log in append */
    if (log_file == NULL)
    {
        perror("fopen"); /* errore apertura log */
        return EXIT_FAILURE;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0); /* AF_INET = usa IPv4, SOCK_STREAM = usa TCP, 0 = protocollo automatico */
    if (server_fd == -1)
    {                     // se errore nella creazione della socket
        perror("socket"); /* stampa errore socket */
        fclose(log_file);
        return EXIT_FAILURE;
    }

    // manteniamo la porta 8080 sempre aperta anche se il server viene chiuso in modo anomalo (Ctrl+C) e non riesce a liberare la porta
    int optval = 1; // valore da assegnare all'opzione SO_REUSEADDR
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
    {
        perror("setsockopt"); /* stampa errore setsockopt */
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));    /* riempie di 0 la variabile server_addr, dato che potrebbe contenere dati rimasti in memoria*/
    server_addr.sin_family = AF_INET;                /* famiglia IPv4 */
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); /* ascolta su ogni IP del pc locale*/
    server_addr.sin_port = htons(SERVER_PORT);       /* porta in formato rete */

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {                   // associazione del socket a porta e ip:
        perror("bind"); /* stampa errore bind */
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) == -1)
    {                     /* mette il server in ascolto */
        perror("listen"); /* stampa errore listen */
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    printf("Coordinatore multi-thread in ascolto sulla porta %d...\n", SERVER_PORT);
    printf("Log scritto su %s\n", LOG_FILE_NAME);

    // segnale sigint
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;

    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction"); /* stampa errore sigaction */
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    // segnale sigalrm per controllo log ogni LOG_CHECK_INTERVAL secondi
    struct sigaction sa_alarm;

    memset(&sa_alarm, 0, sizeof(sa_alarm));
    sa_alarm.sa_handler = handle_sigalrm;

    if (sigaction(SIGALRM, &sa_alarm, NULL) == -1)
    {
        perror("sigaction SIGALRM");
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }
    alarm(LOG_CHECK_INTERVAL); // imposta allarme per il primo controllo del log dopo LOG_CHECK_INTERVAL secondi

    struct sigaction sa_pipe;

    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = handle_sigpipe;

    /* La disconnessione del producer viene rilevata con recv() == 0.
     * SIGPIPE e' gestito per evitare terminazioni anomale se in futuro
     * il server scrive su una socket gia' chiusa.
     */

    if (sigaction(SIGPIPE, &sa_pipe, NULL) == -1)
    {
        perror("sigaction SIGPIPE");
        close(server_fd);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    while (server_running) // rimaniamo in ascolto per sempre, finché non viene interrotto il processo (Ctrl+C)
    {
        int client_fd;                                   /* socket del client che si connette al server */
        struct sockaddr_in client_addr;                  /* indirizzo del client */
        socklen_t client_addr_len = sizeof(client_addr); /* lunghezza dell'indirizzo del client */
        pthread_t thread_id;                             /* id del thread per gestire il client */

        if (log_check)
        {
            log_check = 0;
            new_log();
        }

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len); /* accetta un client */
        if (client_fd == -1)
        {
            if (!server_running)
            { // se l'errore è dovuto a server in chiusura, esci dal ciclo senza stampare errore
                break;
            }
            if (errno == EINTR)
            { // se l'errore è dovuto a un segnale (come SIGINT o SIGALRM), continua ad accettare altri client senza stampare errore
                continue;
            }

            perror("accept"); /* errore accept */
            close(server_fd);
            fclose(log_file);
            return EXIT_FAILURE;
        }

        if (log_check)
        {
            log_check = 0;
            new_log();
        }

        printf("Produttore connesso da %s:%d\n",
               inet_ntoa(client_addr.sin_addr), /* IP client leggibile */
               ntohs(client_addr.sin_port));    /* porta client leggibile */

        // crea un thread per gestire il client connesso
        int *pclient = malloc(sizeof(int)); // alloca memoria per il socket del client cosi che ogni thread abbia la
                                            // sua memoria separata per il socket del client, evitando conflitti tra thread
        if (pclient == NULL)
        {
            perror("malloc"); /* errore malloc */
            close(client_fd);
            continue; // continua ad accettare altri client
        }

        *pclient = client_fd; // assegna il socket del client alla memoria allocata

        pthread_mutex_lock(&thread_mutex);   // prendi lock per modificare active_clients
        active_clients++;                    // incrementa numero di client attivi
        pthread_mutex_unlock(&thread_mutex); // rilascia lock

        if (pthread_create(&thread_id, NULL, handle_client, pclient) != 0)
        {

            pthread_mutex_lock(&thread_mutex);   // prendi lock per modificare active_clients
            active_clients--;                    // decrementa numero di client attivi
            pthread_mutex_unlock(&thread_mutex); // rilascia lock

            perror("pthread_create"); /* errore pthread_create */
            free(pclient);            /* libera la memoria allocata per il socket del client */
            close(client_fd);
            continue; // continua ad accettare altri client
        }
        else
        {
        }
        pthread_detach(thread_id); // detacha il thread per liberare le risorse quando termina
    }

    close(server_fd);

    pthread_mutex_lock(&thread_mutex); // prendi lock per controllare active_clients
    while (active_clients > 0)
    {                                                   // aspetta che tutti i client si disconnettano prima di chiudere il server
        pthread_cond_wait(&thread_cond, &thread_mutex); // aspetta segnale di disconnessione da un client
    }
    pthread_mutex_unlock(&thread_mutex); // rilascia lock

    fclose(log_file);

    printf("server terminato.\n");

    return EXIT_SUCCESS;
}
