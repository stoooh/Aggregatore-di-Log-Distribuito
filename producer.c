#include <arpa/inet.h>  /* funzioni per indirizzi IP e byte order */
#include <netinet/in.h> /* strutture per indirizzi IPv4 */
#include <stdio.h>      /* printf, perror */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, bind, listen, accept, recv */
#include <unistd.h>     /* close */
#include <errno.h>      /* errno per error handling */

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

int is_numero(const char *s)
{
    char *endptr;

    errno = 0;
    strtod(s, &endptr);

    if (s == endptr)
    {
        return 0;
    }

    if (*endptr != '\0')
    {
        return 0;
    }

    if (errno == ERANGE)
    {
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[])
{

    if (argc != 2)
    {
        printf("inserisci ID del produttore come argomento (es: ./producer Producer1)\n");
        return EXIT_FAILURE;
    }

    int sockfd;                     /* socket del produttore */
    struct sockaddr_in server_addr; /* indirizzo del server */
    char buffer[BUFFER_SIZE];       /* dato scritto dall'utente */
    char message[BUFFER_SIZE];      /* messaggio ID,DATO */
    ssize_t bytes_sent;             /* numero di byte inviati ( > 0 (dati inviati), -1 (errore)) */
    char *producer_id = argv[1];

    sockfd = socket(AF_INET, SOCK_STREAM, 0); /* AF_INET = usa IPv4, SOCK_STREAM = usa TCP, 0 = protocollo automatico */
    if (sockfd == -1)
    {                     // se errore nella creazione della socket
        perror("socket"); /* stampa errore socket */
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr)); /* riempie di 0 la variabile server_addr, dato che potrebbe contenere dati rimasti in memoria*/
    server_addr.sin_family = AF_INET;             /* famiglia IPv4 */
    server_addr.sin_port = htons(SERVER_PORT);    /* porta del server in formato rete */

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0)
    {                        // converte l'indirizzo IP da stringa a formato binario
        perror("inet_pton"); /* stampa errore inet_pton */
        close(sockfd);
        return EXIT_FAILURE;
    }

    int connection = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)); // stabilisce la connessione al server
    if (connection == -1)
    {                      // se la connessione fallisce
        perror("connect"); /* stampa errore connect */
        close(sockfd);
        return EXIT_FAILURE;
    }
    printf("Connesso al server %s:%d\n", SERVER_IP, SERVER_PORT);

    while (connection == 0)
    {
        // scrivo un messaggio personalizzato da inviare al server
        printf("Inserisci un dato numerico da inviare al coordinatore: ");
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL)
        {                    // legge un messaggio da standard input
            perror("fgets"); /* stampa errore fgets */
            close(sockfd);
            return EXIT_FAILURE;
        }

        // quando scrivo un messaggio di chiusura della connessione (\q)
        if (strcmp(buffer, "\\q\n") == 0)
        {
            printf("Chiusura della connessione...\n");
            connection = -1; // esce dal ciclo
            continue;        // salta l'invio del messaggio di chiusura al server
        }

        buffer[strcspn(buffer, "\r\n")] = '\0'; // rimuove newline finale

        if (buffer[strspn(buffer, " \t")] == '\0' || !is_numero(buffer))
        { // dato vuoto o solo spazi
            printf("Dato non valido: inserisci un valore numerico.\n");
            continue;
        }

        strcpy(message, producer_id); // inserisce ID
        strcat(message, ",");         // separatore ID,dato
        strcat(message, buffer);      // inserisce dato

        bytes_sent = send(sockfd, message, strlen(message), 0); // invia ID,DATO al server
        if (bytes_sent == -1)
        {
            perror("send"); /* stampa errore send */
            close(sockfd);
            return EXIT_FAILURE;
        }

        printf("Messaggio inviato: %s\n", message);
    }

    close(sockfd);
    printf("Produttore terminato.\n");

    return EXIT_SUCCESS;
}
