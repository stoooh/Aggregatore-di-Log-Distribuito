# Spiegazione delle funzioni del progetto

Questo file spiega il funzionamento delle funzioni principali di `server.c` e `producer.c`.

L'obiettivo non e' spiegare solo cosa fa ogni funzione, ma anche come i pezzi collaborano tra loro: socket, thread, log, mutex e segnali.

## Visione generale

Il progetto ha due programmi:

- `server.c`: il coordinatore/aggregatore. Accetta connessioni TCP, riceve dati dai produttori, scrive il file di log e gestisce segnali.
- `producer.c`: il produttore/client. Si connette al server e invia messaggi nel formato `ID,DATO`.

Il flusso generale e':

```text
producer -> invia "P1,42" -> server -> scrive [TIMESTAMP, P1, 42]
```

Quando un producer chiude la connessione, il server scrive:

```text
[TIMESTAMP, P1, DISCONNECT]
```

# server.c

## Variabili globali importanti

Prima delle funzioni, il server definisce alcune variabili globali.

```c
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
```

`log_file` e' il puntatore al file di log. Siccome tutti i thread devono scrivere nello stesso file, questo puntatore e' globale.

`log_mutex` e' il lock che protegge il file. Serve per evitare che due thread scrivano contemporaneamente nel log.

Senza mutex, due righe potrebbero mescolarsi.

```c
volatile __sig_atomic_t server_running = 1;
volatile sig_atomic_t log_check = 0;
```

`server_running` controlla il ciclo principale del server. Quando arriva `SIGINT`, diventa `0` e il server inizia la chiusura.

`log_check` viene impostata a `1` quando arriva `SIGALRM`. Il main poi controlla questa variabile e decide se ruotare il log.

```c
int active_clients = 0;
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;
```

`active_clients` conta quanti thread/client sono ancora attivi.

`thread_mutex` protegge questo contatore.

`thread_cond` serve al main per aspettare che tutti i thread finiscano prima di chiudere il file di log.

---

## `remove_newline`

```c
void remove_newline(char *text)
{
    int text_clean = strcspn(text, "\r\n");
    text[text_clean] = '\0';
}
```

Questa funzione rimuove il newline finale da una stringa.

Quando ricevi o leggi una stringa, spesso contiene `\n`, cioe' il carattere di invio. Per esempio:

```text
"P1,42\n"
```

La funzione cerca la prima posizione in cui compare `\r` oppure `\n`:

```c
int text_clean = strcspn(text, "\r\n");
```

Poi sostituisce quel carattere con `'\0'`, cioe' fine stringa:

```c
text[text_clean] = '\0';
```

Risultato:

```text
"P1,42\n" -> "P1,42"
```

Nel server viene usata prima di scrivere nel log, cosi' il formato resta pulito.

---

## `get_timestamp`

```c
void get_timestamp(char *timestamp, size_t size)
{
    time_t now = time(NULL);
    struct tm local_time;

    localtime_r(&now, &local_time);
    strftime(timestamp, size, "%Y-%m-%d %H:%M:%S", &local_time);
}
```

Questa funzione genera il timestamp da mettere nel log.
Prima prende il tempo corrente:

```c
time_t now = time(NULL);
```

Poi lo converte in data locale:

```c
localtime_r(&now, &local_time);
```

Qui viene usata `localtime_r` invece di `localtime` perche' il server usa thread. `localtime_r` e' piu' adatta in programmi concorrenti.

Infine formatta la data:

```c
strftime(timestamp, size, "%Y-%m-%d %H:%M:%S", &local_time);
```

Esempio risultato:

```text
2026-05-26 14:35:10
```

Questo timestamp viene poi usato nelle righe del log:

```text
[2026-05-26 14:35:10, P1, 42]
```

---

## `write_log_message`

```c
void write_log_message(const char *message)
```

Questa funzione scrive nel file di log un messaggio normale ricevuto da un producer.

Il producer invia messaggi nel formato:

```text
ID,DATO
```

Esempio:

```text
P1,42
```

La funzione inizia copiando il messaggio:

```c
snprintf(copy, sizeof(copy), "%s", message);
remove_newline(copy);
```

La copia serve perche' poi la funzione modifica la stringa. Non conviene modificare direttamente `message`.

Poi cerca la virgola:

```c
separator = strchr(copy, ',');
```

Se trova la virgola, divide il messaggio in due parti:

```c
*separator = '\0';
producer_id = copy;
data = separator + 1;
```

Esempio:

```text
copy = "P1,42"
```

Dopo `*separator = '\0'`:

```text
producer_id = "P1"
data = "42"
```

Se invece non trova la virgola, usa:

```c
producer_id = "UNKNOWN";
data = copy;
```

Questa e' una protezione: se arriva un messaggio malformato, il server non crasha.

Poi genera il timestamp:

```c
get_timestamp(timestamp, sizeof(timestamp));
```

La scrittura vera e propria e' protetta dal mutex:

```c
pthread_mutex_lock(&log_mutex);
fprintf(log_file, "[%s, %s, %s]\n", timestamp, producer_id, data);
fflush(log_file);
pthread_mutex_unlock(&log_mutex);
```

Questa e' una sezione critica.

Solo un thread alla volta puo' eseguirla. Questo rispetta la traccia:

```text
ogni thread deve ottenere l'uso esclusivo del file di log mediante un lock di scrittura
```

---

## `write_logout_message`

```c
void write_logout_message(const char *producer_id)
{
    char timestamp[32];

    get_timestamp(timestamp, sizeof(timestamp));
    pthread_mutex_lock(&log_mutex);
    fprintf(log_file, "[%s, %s, DISCONNECT]\n", timestamp, producer_id);
    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}
```

Questa funzione scrive nel log la disconnessione di un producer.

Viene chiamata quando il thread rileva che il client ha chiuso la connessione.

La riga prodotta e':

```text
[TIMESTAMP, ID_MITTENTE, DISCONNECT]
```

Anche qui la scrittura e' protetta dal mutex:

```c
pthread_mutex_lock(&log_mutex);
...
pthread_mutex_unlock(&log_mutex);
```

Questo e' necessario perche' anche una disconnessione puo' avvenire mentre un altro thread sta scrivendo un dato normale.

---

## `handle_sigint`

```c
void handle_sigint(int sigint)
{
    (void)sigint;
    server_running = 0;
}
```

Questa funzione gestisce `SIGINT`, cioe' il segnale che arriva quando premi `CTRL+C`.

Dentro un signal handler bisogna fare poche cose. Per questo il codice non chiude direttamente file, socket o mutex.

Fa solo:

```c
server_running = 0;
```

Il main, che ha questo ciclo:

```c
while (server_running)
```

capisce che deve uscire dal ciclo e iniziare la terminazione controllata.

Questa scelta e' importante: l'handler non fa operazioni pericolose, ma comunica al programma principale che deve fermarsi.

---

## `handle_sigalrm`

```c
void handle_sigalrm(int sigalrm)
{
    (void)sigalrm;
    log_check = 1;
    alarm(LOG_CHECK_INTERVAL);
}
```

Questa funzione gestisce `SIGALRM`.

`SIGALRM` arriva dopo un timer impostato con:

```c
alarm(LOG_CHECK_INTERVAL);
```

Quando arriva il segnale, l'handler imposta:

```c
log_check = 1;
```

Non controlla direttamente il file di log, perche' dentro un signal handler non conviene fare operazioni complesse come `fseek`, `ftell`, `fclose`, `rename`, `pthread_mutex_lock`.

Poi riavvia il timer:

```c
alarm(LOG_CHECK_INTERVAL);
```

Quindi il controllo viene richiesto periodicamente.

Il main poi vede:

```c
if (log_check)
{
    log_check = 0;
    new_log();
}
```

e chiama la funzione che controlla davvero il file.

---

## `handle_sigpipe`

```c
void handle_sigpipe(int sigpipe)
{
    (void)sigpipe;
}
```

Questa funzione gestisce `SIGPIPE`.

Tecnicamente `SIGPIPE` arriva quando un processo prova a scrivere su una socket gia' chiusa dall'altra parte.

Nel progetto il server principalmente riceve dati dai producer, quindi la disconnessione reale viene rilevata con:

```c
recv(...) == 0
```

Pero' la traccia richiede la gestione di `SIGPIPE`, quindi il programma registra comunque un handler.

Questo handler evita che il server termini in modo anomalo se in futuro dovesse scrivere su una socket chiusa.

Il log `DISCONNECT`, invece, viene scritto nel thread quando `recv` ritorna `0`.

---

## `new_log`

```c
void new_log()
```

Questa funzione controlla la dimensione del file di log e, se supera il limite, crea un nuovo file.

Prima prende il mutex:

```c
pthread_mutex_lock(&log_mutex);
```

Questo e' fondamentale perche' non deve ruotare il file mentre un thread sta scrivendo.

Poi va alla fine del file:

```c
fseek(log_file, 0, SEEK_END);
long log_size = ftell(log_file);
```

`ftell` restituisce la posizione corrente nel file. Siccome il puntatore e' stato spostato alla fine, questa posizione equivale alla dimensione del file.

Poi controlla:

```c
if (log_size >= LOG_SIZE_LIMIT)
```

Se il file e' troppo grande, lo chiude:

```c
fclose(log_file);
```

Prepara un nuovo nome:

```c
char timestamp[32];
get_timestamp(timestamp, sizeof(timestamp));
char new_log_name[64];
snprintf(new_log_name, sizeof(new_log_name), "log_%s.log", timestamp);
```

Poi rinomina il vecchio file:

```c
rename(LOG_FILE_NAME, new_log_name)
```

E riapre un nuovo `server.log`:

```c
log_file = fopen(LOG_FILE_NAME, "a");
```

Alla fine rilascia il mutex:

```c
pthread_mutex_unlock(&log_mutex);
```

In pratica:

```text
server.log troppo grande
-> viene rinominato
-> nasce un nuovo server.log
```

Questo implementa la log rotation richiesta dalla traccia.

---

## `handle_client`

```c
void *handle_client(void *arg)
```

Questa e' la funzione eseguita da ogni thread del server.

Ogni volta che il server accetta un producer, crea un thread che esegue questa funzione.

Il thread riceve il file descriptor del client tramite `arg`:

```c
int client_fd = *(int *)arg;
free(arg);
```

`arg` e' un `void *`, cioe' un puntatore generico. Per usarlo come socket, viene convertito in `int *` e dereferenziato.

Subito dopo viene fatto `free(arg)` per liberare la memoria allocata dal main.

Poi prepara:

```c
char buffer[BUFFER_SIZE];
ssize_t bytes_read;
char producer_id[BUFFER_SIZE] = "UNKNOWN";
```

`buffer` contiene i messaggi ricevuti.

`bytes_read` contiene il risultato di `recv`.

`producer_id` conserva l'ultimo ID noto del producer. Parte da `UNKNOWN` per evitare stringhe non inizializzate.

La ricezione comincia qui:

```c
bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
```

Poi il thread continua finche riceve dati:

```c
while (bytes_read > 0)
```

Dentro il ciclo, il buffer viene trasformato in stringa:

```c
buffer[bytes_read] = '\0';
```

Poi cerca l'ID:

```c
char *separator = strchr(buffer, ',');
if (separator != NULL)
{
    *separator = '\0';
    snprintf(producer_id, sizeof(producer_id), "%s", buffer);
    *separator = ',';
}
```

Qui succede una cosa furba:

1. trova la virgola in `P1,42`;
2. la sostituisce temporaneamente con `\0`;
3. copia `P1` dentro `producer_id`;
4. rimette la virgola al suo posto.

Serve per ricordare l'ID quando il producer si disconnette.

Poi il messaggio viene stampato e scritto nel log:

```c
printf("Messaggio ricevuto: %s\n", buffer);
write_log_message(buffer);
```

Poi il thread aspetta un altro messaggio dallo stesso producer:

```c
bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
```

Quando il ciclo finisce ci sono due possibilita':

```c
if (bytes_read == -1)
```

Errore di ricezione.

Oppure:

```c
else
{
    printf("%s ha chiuso la connessione.\n", producer_id);
    write_logout_message(producer_id);
}
```

Qui `recv` ha restituito `0`, quindi il producer ha chiuso la connessione. Il server scrive `DISCONNECT`.

Alla fine:

```c
close(client_fd);
```

chiude la socket specifica di quel producer.

Poi aggiorna il numero di client attivi:

```c
pthread_mutex_lock(&thread_mutex);
active_clients--;
...
pthread_mutex_unlock(&thread_mutex);
```

Se non ci sono piu client attivi:

```c
pthread_cond_signal(&thread_cond);
```

sveglia il main, che potrebbe essere in attesa durante la chiusura controllata.

Infine:

```c
return NULL;
```

termina il thread.

---

## `main` del server

```c
int main(void)
```

Il `main` del server prepara tutto il coordinatore.

### Apertura del log

```c
log_file = fopen(LOG_FILE_NAME, "a");
```

Apre `server.log` in append. Append significa che le nuove righe vengono aggiunte alla fine.

Se fallisce:

```c
if (log_file == NULL)
```

il programma termina.

### Creazione socket

```c
server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

Crea una socket TCP IPv4.

- `AF_INET`: IPv4;
- `SOCK_STREAM`: TCP;
- `0`: protocollo automatico.

### Riuso rapido della porta

```c
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))
```

Permette di riavviare subito il server sulla stessa porta senza aspettare che il sistema operativo la liberi completamente.

Questo soddisfa il requisito:

```text
riuso rapido della coppia IP:PORT
```

### Preparazione indirizzo

```c
memset(&server_addr, 0, sizeof(server_addr));
server_addr.sin_family = AF_INET;
server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
server_addr.sin_port = htons(SERVER_PORT);
```

Qui il server prepara indirizzo e porta.

`INADDR_ANY` significa: ascolta su tutti gli indirizzi IP della macchina.

`htons` e `htonl` convertono valori nel formato della rete.

### Bind

```c
bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr))
```

Associa la socket alla porta `8080`.

Senza `bind`, il server non avrebbe una porta su cui ricevere connessioni.

### Listen

```c
listen(server_fd, BACKLOG)
```

Mette la socket in ascolto.

Da questo momento il server puo' accettare connessioni dai producer.

### Registrazione segnali

Il server registra tre segnali.

`SIGINT`:

```c
sa.sa_handler = handle_sigint;
sigaction(SIGINT, &sa, NULL);
```

Serve per `CTRL+C`.

`SIGALRM`:

```c
sa_alarm.sa_handler = handle_sigalrm;
sigaction(SIGALRM, &sa_alarm, NULL);
alarm(LOG_CHECK_INTERVAL);
```

Serve per il controllo periodico del log.

`SIGPIPE`:

```c
sa_pipe.sa_handler = handle_sigpipe;
sigaction(SIGPIPE, &sa_pipe, NULL);
```

Serve per evitare terminazioni anomale se il server scrive su una socket chiusa.

### Ciclo principale

Il cuore del server e':

```c
while (server_running)
```

Finche il server e attivo, controlla il log:

```c
if (log_check)
{
    log_check = 0;
    new_log();
}
```

Poi aspetta un client:

```c
client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
```

`accept` restituisce una nuova socket, `client_fd`, dedicata a quel producer.

Se `accept` fallisce, il codice distingue tre casi:

```c
if (!server_running)
{
    break;
}
if (errno == EINTR)
{
    continue;
}
```

- se il server sta chiudendo, esce dal ciclo;
- se la system call e' stata interrotta da un segnale, ricomincia;
- altrimenti e' un errore reale.

### Creazione thread

Quando un client e' stato accettato, il main alloca memoria per passare `client_fd` al thread:

```c
int *pclient = malloc(sizeof(int));
*pclient = client_fd;
```

Poi incrementa il contatore:

```c
pthread_mutex_lock(&thread_mutex);
active_clients++;
pthread_mutex_unlock(&thread_mutex);
```

E crea il thread:

```c
pthread_create(&thread_id, NULL, handle_client, pclient)
```

Ogni producer viene quindi gestito da un thread separato.

Infine:

```c
pthread_detach(thread_id);
```

Il thread viene detached, cioe' quando termina il sistema libera automaticamente le sue risorse.

### Chiusura controllata

Quando `server_running` diventa `0`, il main esce dal ciclo e chiude la socket di ascolto:

```c
close(server_fd);
```

Poi aspetta che tutti i thread finiscano:

```c
pthread_mutex_lock(&thread_mutex);
while (active_clients > 0)
{
    pthread_cond_wait(&thread_cond, &thread_mutex);
}
pthread_mutex_unlock(&thread_mutex);
```

Solo dopo chiude il file di log:

```c
fclose(log_file);
```

Questo rispetta l'idea della terminazione controllata: non chiude il log mentre i thread potrebbero ancora scrivere.

# producer.c

## `is_numero`

```c
int is_numero(const char *s)
```

Questa funzione controlla se la stringa inserita dall'utente rappresenta un numero valido.

Usa:

```c
strtod(s, &endptr);
```

`strtod` prova a convertire la stringa in un numero double.

`endptr` indica dove si e' fermata la conversione.

Prima azzera `errno`:

```c
errno = 0;
```

Poi controlla se non e' stato letto nessun numero:

```c
if (s == endptr)
{
    return 0;
}
```

Esempio non valido:

```text
abc
```

Poi controlla se dopo il numero ci sono caratteri extra:

```c
if (*endptr != '\0')
{
    return 0;
}
```

Esempio non valido:

```text
123abc
```

Poi controlla overflow o underflow:

```c
if (errno == ERANGE)
{
    return 0;
}
```

Se tutti i controlli passano:

```c
return 1;
```

Quindi la stringa e' considerata numerica.

---

## `main` del producer

```c
int main(int argc, char *argv[])
```

Il producer riceve l'ID da riga di comando.

Esempio:

```bash
./producer P1
```

Qui `P1` e' `argv[1]`.

### Controllo argomenti

```c
if (argc != 2)
{
    printf("inserisci ID del produttore come argomento (es: ./producer Producer1)\n");
    return EXIT_FAILURE;
}
```

Il programma vuole esattamente un ID. Se manca, termina.

### Variabili principali

```c
int sockfd;
struct sockaddr_in server_addr;
char buffer[BUFFER_SIZE];
char message[BUFFER_SIZE];
ssize_t bytes_sent;
char *producer_id = argv[1];
```

- `sockfd`: socket del producer;
- `server_addr`: indirizzo del server;
- `buffer`: dato scritto dall'utente;
- `message`: messaggio finale `ID,DATO`;
- `bytes_sent`: risultato della `send`;
- `producer_id`: ID del produttore.

### Creazione socket

```c
sockfd = socket(AF_INET, SOCK_STREAM, 0);
```

Crea una socket TCP IPv4.

Se fallisce:

```c
if (sockfd == -1)
```

stampa errore e termina.

### Preparazione indirizzo server

```c
memset(&server_addr, 0, sizeof(server_addr));
server_addr.sin_family = AF_INET;
server_addr.sin_port = htons(SERVER_PORT);
```

Azzera la struttura, imposta IPv4 e porta del server.

Poi converte l'IP:

```c
inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr)
```

`SERVER_IP` vale:

```c
"127.0.0.1"
```

Quindi il producer si connette al server sulla stessa macchina.

### Connessione

```c
int connection = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
```

Questa chiamata stabilisce la connessione TCP col server.

Se fallisce:

```c
if (connection == -1)
```

chiude la socket e termina.

### Ciclo di invio

```c
while (connection == 0)
```

Finche la connessione e attiva, il producer chiede dati all'utente.

Legge input con:

```c
fgets(buffer, BUFFER_SIZE, stdin)
```

Se l'utente scrive:

```text
\q
```

allora il producer non invia nulla e si prepara a chiudere:

```c
connection = -1;
continue;
```

### Pulizia e validazione dato

```c
buffer[strcspn(buffer, "\r\n")] = '\0';
```

Rimuove il newline finale.

Poi controlla:

```c
if (buffer[strspn(buffer, " \t")] == '\0' || !is_numero(buffer))
```

Questo significa:

- se il dato e' vuoto;
- oppure contiene solo spazi;
- oppure non e' numerico;

allora non viene inviato.

### Costruzione messaggio

```c
strcpy(message, producer_id);
strcat(message, ",");
strcat(message, buffer);
```

Queste tre righe costruiscono il messaggio finale:

```text
ID,DATO
```

Esempio:

```text
producer_id = "P1"
buffer = "42"
message = "P1,42"
```

### Invio al server

```c
bytes_sent = send(sockfd, message, strlen(message), 0);
```

Invia il messaggio al server tramite TCP.

Se `send` fallisce:

```c
if (bytes_sent == -1)
```

stampa errore, chiude la socket e termina.

### Chiusura

Quando il ciclo finisce:

```c
close(sockfd);
printf("Produttore terminato.\n");
return EXIT_SUCCESS;
```

Il producer chiude la socket e termina correttamente.

# Collegamento tra producer e server

Il producer costruisce:

```c
strcpy(message, producer_id);
strcat(message, ",");
strcat(message, buffer);
```

Il server riceve:

```c
bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
```

Il server scrive:

```c
write_log_message(buffer);
```

La funzione di log separa:

```c
separator = strchr(copy, ',');
```

e produce:

```text
[TIMESTAMP, ID, DATO]
```

Quando il producer chiude:

```c
close(sockfd);
```

il server vede:

```c
recv(...) == 0
```

e scrive:

```text
[TIMESTAMP, ID, DISCONNECT]
```

# Riassunto finale

Le funzioni principali hanno questi ruoli:

- `remove_newline`: pulisce stringhe togliendo newline.
- `get_timestamp`: crea data e ora per il log.
- `write_log_message`: scrive righe normali nel log.
- `write_logout_message`: scrive righe `DISCONNECT`.
- `handle_sigint`: segnala al server di terminare.
- `handle_sigalrm`: chiede il controllo periodico del log.
- `handle_sigpipe`: evita terminazioni anomale per socket chiuse.
- `new_log`: controlla dimensione log e fa rotazione.
- `handle_client`: gestisce un producer in un thread.
- `main` del server: configura socket, segnali, thread e shutdown.
- `is_numero`: valida il dato numerico inserito.
- `main` del producer: connette il client e invia `ID,DATO`.
