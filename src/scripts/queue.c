#include <pthread.h>
#include <unistd.h>

#include "../array.h"
#include "../map.h"

struct task {
    struct task    *next;

    enum task_type {
        SAY_HELLO = 1,
        TIME_TO_WRAP_UP,
    }               type;
};

struct queue {
    pthread_mutex_t mutex;
    pthread_cond_t  ready; // We will broadcast when there are tasks.

    Memory_context *context;

    struct task    *head;
    struct task    *tail;
};

void say_hello(char *thread_name)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&mutex);

    printf("Hello from thread %s!\n", thread_name);

    pthread_mutex_unlock(&mutex);

    sleep(1);
}

void *thread_start(void *arg)
{
    struct queue *queue = arg;

    pthread_t thread_id = pthread_self();
    u64 thread_hash = hash_bytes(&thread_id, sizeof(thread_id));

    char thread_name[20];
    snprintf(thread_name, sizeof(thread_name), "%04lx", thread_hash & (u64)0xffff);

    while (true) {
        // Wait for there to be a task.
        pthread_mutex_lock(&queue->mutex);
        while (!queue->head)  pthread_cond_wait(&queue->ready, &queue->mutex);

        struct task task = *queue->head; // Copy the task to this thread's stack.

        // Remove the task from the queue, unless it's time to wrap up,
        // in which case we'll leave it for the other threads to see.
        if (task.type != TIME_TO_WRAP_UP) {
            dealloc(queue->head, queue->context);
            queue->head = task.next;
            if (!task.next)  queue->tail = NULL;
        }

        pthread_mutex_unlock(&queue->mutex);

        // If it's time to wrap up, break out of this loop.
        if (task.type == TIME_TO_WRAP_UP)  break;

        assert(task.type == SAY_HELLO);

        say_hello(thread_name);
    }

    return NULL;
}

struct task *add_task(struct queue *queue)
// This assumes you've already locked the queue's mutex.
{
    struct task *task = New(struct task, queue->context);

    if (queue->head) {
        queue->tail->next = task;
        queue->tail = task;
    } else {
        queue->head = task;
        queue->tail = task;
    }

    return task;
}

int main(int argc, char **argv)
{
    Memory_context *main_context = new_context(NULL);

    // Get the number of threads from the command-line argument. Default to 1.
    int num_threads = 1;
    if (argc > 1) {
        num_threads = strtol(argv[1], NULL, 10);
    }
    assert(0 < num_threads && num_threads < INT32_MAX);

    Array(pthread_t) threads = {.context = main_context};
    array_reserve(&threads, num_threads);

    Memory_context *shared_context = new_context(NULL);

    // Initialise the queue.
    struct queue queue = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready = PTHREAD_COND_INITIALIZER,
        .context = shared_context,
    };

    // Start the threads.
    printf("Starting %d threads.\n", num_threads);

    bool done = false;

    while (!done) {
        // Wait for console input.
        char input_buffer[1024] = {0};
        char *input = fgets(input_buffer, sizeof(input_buffer), stdin);

        char *after_parsed = NULL;
        long parsed_number = strtol(input, &after_parsed, 10);
        if (after_parsed == input) {
            printf("Please enter a number.\n");
            continue;
        }

        pthread_mutex_lock(&queue.mutex);

        if (parsed_number < 0) {
            // We parsed a negative number. It's time to wrap up.
            *add_task(&queue) = (struct task){.type = TIME_TO_WRAP_UP};

            done = true;
        } else {
            // Take the parsed number as the number of tasks to add.
            for (int i = 0; i < parsed_number; i++) {
                *add_task(&queue) = (struct task){.type = SAY_HELLO};
            }
        }

        pthread_cond_broadcast(&queue.ready);
        pthread_mutex_unlock(&queue.mutex);
    }

    printf("Joined %d threads.\n", num_threads);

    // Now that we've joined all the other threads, we can operate on the queue without locking.
    assert(queue.head->type == TIME_TO_WRAP_UP);
    assert(queue.head->next == NULL);
    assert(queue.tail == queue.head);

    dealloc(queue.head, shared_context);

    free_context(main_context);
    free_context(shared_context);

    return 0;
}
