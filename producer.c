#include <arpa/inet.h> 
#include <netinet/in.h> 
#include <stdio.h>      
#include <stdlib.h>    
#include <string.h>    
#include <sys/socket.h> 
#include <unistd.h>     

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

int is_numero(const char *s)
{
    char *p; // puntatore per indicare la posizione del primo carattere non convertito da strtod

    //strtod tramite il puntatore p CHE SCORRE sulla stringa s, converte fino a dove ci sono numeri, e p punta alla prima occorrenza di un char alfanumerico
    strtod(s, &p); // converte la stringa in un numero double, e endptr punta alla prima posizione dopo il numero convertito

    if (s == p) return 0;//se la stringa s inizia per un carattere 

    if (*p != '\0') return 0;//se ci sono altri caratteri (non numerici), allora non è un numero valido

    return 1;
}

int main(int argc, char *argv[])
{

    if (argc != 2)     //se non viene passato l'id del produttore da riga di comando
    {
        printf("inserisci ID del produttore come argomento (es: ./producer Producer1)\n");
        return EXIT_FAILURE;
    }

    int sock_producer;              // socket del produttore 
    struct sockaddr_in server_addr; // indirizzo del server 
    char buffer[BUFFER_SIZE];       // dato scritto dall'utente
    char message[BUFFER_SIZE];      // messaggio ID,DATO
    ssize_t bytes_sent;             // numero di byte inviati ( > 0 (dati inviati), -1 (errore))
    char *producer_id = argv[1];

    sock_producer = socket(AF_INET, SOCK_STREAM, 0); // AF_INET = usa IPv4, SOCK_STREAM = usa TCP, 0 = protocollo automatico
    if (sock_producer == -1)
    {                     // se errore nella creazione della socket
        perror("socket"); // stampa errore socket
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr)); // riempie di 0 la variabile server_addr, dato che potrebbe contenere dati rimasti in memoria
    server_addr.sin_family = AF_INET;             // famiglia IPv4
    server_addr.sin_port = htons(SERVER_PORT);    // porta del server in formato rete (uso htons per valori corti)

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0)
    {                        // converte l'indirizzo IP da stringa a formato binario
        perror("inet_pton"); // stampa errore inet_pton
        close(sock_producer);
        return EXIT_FAILURE;
    }

    int connection = connect(sock_producer, (struct sockaddr *)&server_addr, sizeof(server_addr)); // stabilisce la connessione al server
    if (connection == -1)
    {                      // se la connessione fallisce
        perror("connect"); // stampa errore connect
        close(sock_producer);
        return EXIT_FAILURE;
    }
    printf("Connesso al server %s:%d\n", SERVER_IP, SERVER_PORT);

    while (connection == 0)
    {
        // scrivo un messaggio personalizzato da inviare al server
        printf("Inserisci un dato numerico da inviare al coordinatore: ");
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL)
        {                    // legge un messaggio da standard input
            perror("fgets"); // stampa errore fgets
            close(sock_producer);
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

        bytes_sent = send(sock_producer, message, strlen(message), 0); // invia ID,DATO al server
        if (bytes_sent == -1)
        {
            perror("send"); 
            close(sock_producer);
            return EXIT_FAILURE;
        }

        printf("Messaggio inviato: %s\n", message);
    }

    close(sock_producer);
    printf("Produttore terminato.\n");

    return EXIT_SUCCESS;
}
