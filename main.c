#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>


#define MEMORY_SIZE 4096  // Taille de la mémoire partagée en octets (32 Ko)
#define TOTAL_MEMORY_SIZE  (MEMORY_SIZE + sizeof(size_t))

// used_memory is now stored within shared_memory
#define USED_MEMORY_OFFSET 0
#define MESSAGE_OFFSET (USED_MEMORY_OFFSET + sizeof(size_t)) // Start of message data


// Globals
char file_w[256], file_r[256];
pid_t child_pid; // Track the child process PID
pid_t parent_pid; // Track the parent process PID
bool termination_message_sent = false;

int shmid; // Shared memory ID

// Pointeur vers la mémoire partagée et taille des messages stockées dedans
char *shared_memory;

// Variable globale pour indiquer si le mode manuel / bot est activé
bool bot_mode = false;
bool manual_mode = false;

void display_bot_manuel(const char* pseudo_utilisateur, const char* msg_of_user, bool bot_mode){
    if(!bot_mode){
        printf("[\x1B[4m%s\x1B[0m] %s\n", pseudo_utilisateur, msg_of_user);
    }
    else{
        printf("[%s] %s\n", pseudo_utilisateur, msg_of_user);
        fflush(stdout);

    }
}

int pseudos_verification(const char* pseudo_utilisateur, const char* pseudo_destinataire, size_t size_pseudo, const char* non_valid_character) {
    if (strlen(pseudo_utilisateur) > size_pseudo || strlen(pseudo_destinataire) >= size_pseudo) {
        fprintf(stderr, "Error: The pseudo length exceeds the limit of %zu characters\n", size_pseudo);
        return 2;
    }

    for (size_t i = 0; i < strlen(pseudo_utilisateur); i++) {
        if (strchr(non_valid_character, pseudo_utilisateur[i])) {
            fprintf(stderr, "You have used a forbidden character\n");
            return 3;
        }
    }
    for (size_t i = 0; i < strlen(pseudo_destinataire); i++) {
        if (strchr(non_valid_character, pseudo_destinataire[i])) {
            fprintf(stderr, "You have used a forbidden character\n");
            return 3;
        }
    }

    if (strcmp(pseudo_utilisateur, ".") == 0 || strcmp(pseudo_utilisateur, "..") == 0 ||
        strcmp(pseudo_destinataire, ".") == 0 || strcmp(pseudo_destinataire, "..") == 0) {
        fprintf(stderr, "Error: Your pseudo cannot be . or ..\n");
        return 3;
    }
    return 0;
}

int create_pipe(const char* file){

    if (mkfifo(file, 0666) == -1){
        if (errno != EEXIST){
            fprintf(stderr, "Error: Unable to create the pipe.\n");
            return 1;
        }
    }
    return 0;

}


// Cleanup function to unlink pipes
void quit_chat_room() {
    if (access(file_w, F_OK) == 0) {
        unlink(file_w);
    }
    if (access(file_r, F_OK) == 0) {
        unlink(file_r);
    }
    exit(0);
}

void display_pending_messages(bool from_interrupt) {
    size_t *used_memory_ptr = (size_t *)shared_memory; // Access `used_memory`
    char *message_start = shared_memory + MESSAGE_OFFSET; // Message area
    if (*used_memory_ptr == 0 && from_interrupt) {
        printf("No messages in shared memory.\n");
        return;
    } else if (*used_memory_ptr > 0) {
        // Iterate through the buffer up to `*used_memory_ptr`
        size_t remaining = *used_memory_ptr;
        char *current = message_start;

        while (remaining > 0) {
            size_t message_len = strnlen(current, remaining); // Get the length of the current message
            if (message_len == remaining) {
                fprintf(stderr, "Corrupt message detected.\n");
                break;
            }
            // Safeguard: stop if a completely empty message is encountered
            printf("%.*s", (int)message_len, current); // Print the current message

            // Move to the next message
            remaining -= (message_len + 1);
            current += (message_len + 1);
        }
        // Clear the message area
        memset(message_start, 0, MEMORY_SIZE - MESSAGE_OFFSET);
        *used_memory_ptr = 0; // Reset `used_memory`
    }
    return;
}

void handle_sigint_manual(int sig) {
    (void)sig; // Ignorer sig si non utilisé
    if (getpid() == parent_pid) {
        display_pending_messages(true);
    }

}

// Signal handler for SIGINT
void handle_sigint_bot(int sig) {
   (void) sig;
    if (getpid() == parent_pid) {
        if (!termination_message_sent) {
            termination_message_sent = true;
            const char *msg = "Exiting program.\n";
            write(STDOUT_FILENO, msg, strlen(msg));
        }
    }
    quit_chat_room();

}
void error_and_exit(const char *errmessage){
    perror(errmessage);
    quit_chat_room();
}


void setup_signal_handler_bot() {

    struct sigaction sa2;
    sa2.sa_handler = handle_sigint_bot;
    sa2.sa_flags = 0;
    sigemptyset(&sa2.sa_mask);

    if (sigaction(SIGINT, &sa2, NULL) == -1) {
        perror("Erreur lors de la configuration du gestionnaire de signal");
        exit(EXIT_FAILURE);
    }
}

void setup_signal_handler_man() {

    struct sigaction sa;
    sa.sa_handler = handle_sigint_manual;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Erreur lors de la configuration du gestionnaire de signal");
        exit(EXIT_FAILURE);
    }
}
void handle_sigterm(int sig) {
    (void)sig; // Ignore unused parameter

    wait(NULL);

    printf("Exiting program.\n");
    quit_chat_room();
    shmdt(shared_memory);
    shmctl(shmid, IPC_RMID, NULL);
    exit(0);
}

void sigpipe_handler(int signum) {
    (void) signum;
    fprintf(stderr, "SIGPIPE received, write failed.\n");
}

void setup_sigpipe_handler(){
    struct sigaction sa1;
    sa1.sa_handler = sigpipe_handler;
    sa1.sa_flags = 0;
    sigemptyset(&sa1.sa_mask);
    sigaction(SIGPIPE, &sa1, NULL);
}

// Fonction pour ajouter un message en mémoire partagée
void receive_message(const char *pseudo, const char *message) {
    size_t *used_memory_ptr = (size_t*)shared_memory; // Access used_memory
    char *message_start = shared_memory + MESSAGE_OFFSET; // Message area

    size_t message_len = strlen(pseudo) + strlen(message) + 5; // Space for "[pseudo] message\n"
    char formatted_message[message_len];
    snprintf(formatted_message, message_len, "[%s] %s\n", pseudo, message);

    if (*used_memory_ptr + message_len < MEMORY_SIZE - MESSAGE_OFFSET){
        strncpy(message_start + *used_memory_ptr, formatted_message, message_len);
        *used_memory_ptr += message_len; // Update used_memory
    } else {
        // Memory full: flush and reset
        display_pending_messages(false);
        strncpy(message_start + *used_memory_ptr, formatted_message, message_len);
        *used_memory_ptr += message_len; // Update used_memory
    }
}



int main(int argc, char* argv[]) {

    setup_sigpipe_handler();

    if (argc < 3) {
        fprintf(stderr, "chat pseudo_utilisateur pseudo_destinataire [--bot] [--manuel]\n");
        return 1;
    }

    // Determine the modefr
    for (int i = 0 ; i < argc; i++){
        if(strcmp(argv[i],"--bot")==0){
            bot_mode = true;
        }
    }
    for(int i = 0; i < argc; i++){
        if(strcmp(argv[i], "--manuel")==0){
            manual_mode = true;
        }
    }

    signal(SIGTERM, handle_sigterm);
    if(manual_mode) {
        setup_signal_handler_man();
    }else {
        setup_signal_handler_bot();
    }

    // Initialisation de la mémoire partagée
    key_t key = ftok("shmfile", 65);
    int shmid = shmget(key, MEMORY_SIZE, 0666 | IPC_CREAT);

    if (shmid == -1) {
        perror("Error during the creation of shared memory.");
        return 1;
    }

    shared_memory = (char *) shmat(shmid, NULL, 0);

    if (shared_memory == (char *) -1) {
        perror("Error during the attachment of shared memory.");
        return 1;
    }
    memset(shared_memory, 0, MEMORY_SIZE); // Initialiser la mémoire partagée

    // Initialize `used_memory` at the beginning of `shared_memory`
    size_t *used_memory_ptr = (size_t *) shared_memory;
    *used_memory_ptr = 0;

    char pseudo_utilisateur[35], pseudo_destinataire[35];
    size_t size_pseudo = 30;
    char non_valid_character[] = "/-[]";


    strncpy(pseudo_utilisateur, argv[1], sizeof(pseudo_utilisateur) - 1);
    strncpy(pseudo_destinataire, argv[2], sizeof(pseudo_destinataire) - 1);
    pseudo_utilisateur[sizeof(pseudo_utilisateur) - 1] = '\0';
    pseudo_destinataire[sizeof(pseudo_destinataire) - 1] = '\0';


    int check_pseudo = pseudos_verification(pseudo_utilisateur, pseudo_destinataire, size_pseudo, non_valid_character);
    if (check_pseudo != 0) {
        return check_pseudo;
    }


    snprintf(file_w, sizeof(file_w), "/tmp/%s-%s.chat", pseudo_utilisateur, pseudo_destinataire);
    snprintf(file_r, sizeof(file_r), "/tmp/%s-%s.chat", pseudo_destinataire, pseudo_utilisateur);

    if (create_pipe(file_w) != 0 || create_pipe(file_r) != 0) {
        error_and_exit("Pipes weren't created properly.");
        return 2;
    }

    parent_pid = getpid(); // Store parent process ID

    child_pid = fork();

    if (child_pid < 0) {
        error_and_exit("fork() failed. Exiting program...");
        return 10;
    } else if (child_pid == 0) {

        // Child process
        int fd_r = open(file_r, O_RDONLY);
        if (fd_r == -1) {
            error_and_exit("Error: opening the pipe for reading.");
            return 4;
        }
        char msg_reçu[200];
        while (1){
            ssize_t bytes_read = read(fd_r, msg_reçu, sizeof(msg_reçu));

            if (bytes_read > 0) {
                if (manual_mode) {
                    // Emet un bip sonore
                    printf("\a");
                    fflush(stdout); // Assurez-vous que le caractère est immédiatement écri
                    receive_message(pseudo_destinataire, msg_reçu);
                } else { //mode bot
                    display_bot_manuel(pseudo_destinataire, msg_reçu, bot_mode);
                }
            } else if (bytes_read == 0) {
                if (bot_mode) {
                    kill(parent_pid, SIGINT); // Signal parent to terminate
                } else {
                    kill(parent_pid, SIGTERM);
                }
                close(fd_r);
                exit(0);
            } else if (bytes_read < 0 && errno != EINTR) {
                perror("Error reading from pipe");
                break;
            }
        }

    }else{
        // Parent process
        int fd_w = open(file_w, O_WRONLY);
        if (fd_w == -1) {
            error_and_exit("Error: opening the pipe for writing.");
            return 4;
        }   
        char msg_sent[200];
        while (1) {
            fgets(msg_sent, 200, stdin);
            msg_sent[strlen(msg_sent) - 1] = '\0';
            display_bot_manuel(pseudo_utilisateur, msg_sent, bot_mode);
            if (strcmp(msg_sent, "exit") == 0) {
                if (bot_mode){
                    kill(child_pid, SIGINT);
                    kill(parent_pid, SIGINT);
                     break;
                }
                if (manual_mode){
                    kill(child_pid, SIGTERM);
                    break;
                }

            }

                if (write(fd_w, msg_sent, strlen(msg_sent) + 1) < 0) {
                    perror("Error");
                    close(fd_w);
                    break;
                } else {
                    if (manual_mode) {
                        display_pending_messages(false);
                    }
                }
            }

            close(fd_w);
            wait(NULL); // Wait for the child to terminate
            quit_chat_room();
        }
        shmdt(shared_memory);
        shmctl(shmid, IPC_RMID, NULL);
        return 0;
    }
