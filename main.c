#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>

#define BUFFER_SIZE 1024

typedef struct {
    int client_socket;
} request_t;

typedef struct {
    request_t *queue;
    int front, rear, count, size;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} request_queue_t;

typedef struct {
    pthread_t *threads;
    request_queue_t queue;
    int thread_pool_size;
} thread_pool_t;

// グローバル変数
int PORT;
int THREAD_POOL_SIZE;
int QUEUE_SIZE;
volatile sig_atomic_t reload_config = 0;
volatile sig_atomic_t terminate_server = 0;

void *worker(void *arg);
void init_queue(request_queue_t *q, int size);
int enqueue(request_queue_t *q, request_t *request);
int dequeue(request_queue_t *q, request_t *request);
void handle_client(int client_socket);
void init_pool(thread_pool_t *pool, int pool_size);
void destroy_pool(thread_pool_t *pool);
void load_config(const char *filename);
void signal_handler(int signal);

// 設定ファイルの読み込み
void load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "PORT=", 5) == 0) {
            PORT = atoi(line + 5);
        } else if (strncmp(line, "THREAD_POOL_SIZE=", 17) == 0) {
            THREAD_POOL_SIZE = atoi(line + 17);
        } else if (strncmp(line, "QUEUE_SIZE=", 11) == 0) {
            QUEUE_SIZE = atoi(line + 11);
        }
    }

    fclose(file);
}

void init_queue(request_queue_t *q, int size) {
    q->queue = malloc(sizeof(request_t) * size);
    q->front = q->rear = q->count = 0;
    q->size = size;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

int enqueue(request_queue_t *q, request_t *request) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->size) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    q->queue[q->rear] = *request;
    q->rear = (q->rear + 1) % q->size;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int dequeue(request_queue_t *q, request_t *request) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    *request = q->queue[q->front];
    q->front = (q->front + 1) % q->size;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_received;

    // 受け取ったリクエストの表示
    bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received < 0) {
        perror("recv");
        close(client_socket);
        return;
    }
    buffer[bytes_received] = '\0'; // Null-terminate the received data
    printf("Received request:\n%s\n", buffer);

    // リクエストメソッドを判定
    if (strncmp(buffer, "CONNECT", 7) == 0) {
        // CONNECTリクエストに対する504 Gateway Timeoutレスポンス
        const char *response =
            "HTTP/1.1 504 Gateway Timeout\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 19\r\n"
            "\r\n"
            "Gateway Timeout";
        send(client_socket, response, strlen(response), 0);
    } else {
        // その他のリクエストに対する200 OKレスポンス
        const char *response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, world!";
        send(client_socket, response, strlen(response), 0);
    }

    // ソケットを閉じる
    close(client_socket);
}

void *worker(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    request_t request;

    while (!terminate_server) {
        dequeue(&pool->queue, &request);
        handle_client(request.client_socket);
    }

    return NULL;
}

void init_pool(thread_pool_t *pool, int pool_size) {
    init_queue(&pool->queue, QUEUE_SIZE);
    pool->threads = malloc(sizeof(pthread_t) * pool_size);
    pool->thread_pool_size = pool_size;

    for (int i = 0; i < pool_size; i++) {
        pthread_create(&pool->threads[i], NULL, worker, pool);
    }
}

void destroy_pool(thread_pool_t *pool) {
    terminate_server = 1;
    for (int i = 0; i < pool->thread_pool_size; i++) {
        pthread_cond_broadcast(&pool->queue.not_empty);
    }
    for (int i = 0; i < pool->thread_pool_size; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
    free(pool->queue.queue);
    pthread_mutex_destroy(&pool->queue.lock);
    pthread_cond_destroy(&pool->queue.not_empty);
    pthread_cond_destroy(&pool->queue.not_full);
}

void signal_handler(int signal) {
    if (signal == SIGHUP) {
        reload_config = 1;
    } else if (signal == SIGTERM) {
        terminate_server = 1;
    }
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    thread_pool_t pool;

    // 設定ファイルの読み込み
    load_config("config.cfg");

    // シグナルハンドラーの設定
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // ソケットの作成
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // サーバーアドレスの設定
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // ソケットをアドレスにバインド
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 接続の待機
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // スレッドプールの初期化
    init_pool(&pool, THREAD_POOL_SIZE);

    printf("Server is listening on port %d...\n", PORT);

    // クライアントからの接続を処理
    while (!terminate_server) {
        if (reload_config) {
            reload_config = 0;
            load_config("config.cfg");
            destroy_pool(&pool);
            init_pool(&pool, THREAD_POOL_SIZE);
        }

        client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) {
            if (terminate_server) {
                break;
            }
            perror("accept");
            close(server_fd);
            destroy_pool(&pool);
            exit(EXIT_FAILURE);
        }

        // リクエストをスレッドプールに追加
        request_t request = { .client_socket = client_socket };
        enqueue(&pool.queue, &request);
    }

    // サーバーソケットを閉じる
    close(server_fd);
    destroy_pool(&pool);
    return 0;
}
