#include <algorithm>  // std::random_shuffle
#include <cassert>  // assert
#include <condition_variable>
#include <cstdlib>  // std::rand, std::srand
#include <deque>
#include <iostream>
#include <mutex>
#include <stdarg.h>  // Functions variable arguments: va_list, va_start, va_end.
#include <thread>
#include <vector>
#include <sstream>


/****************************************************************************************
 * CONSTANTS
 ***************************************************************************************/

const int NUM_CONSUMERS = 8;
const int RANDOM_GENERATOR_SEED = 123456;
const int MAX_SLEEP_TIME_MS = 10;
const int CONSUMER_SLEEP_PART = 4;
const int DATA_START = 30;
const int DATA_END = 45;
const int DATA_LENGTH = DATA_END - DATA_START;


/****************************************************************************************
 * HELPER FUNCTIONS
 ***************************************************************************************/

/* Inefficient algorithm for Fibonacci number calculation for wasting some CPU time. */
int fibonacci(int n) {
    assert (n >= 1);
    return n <= 2 ? 1 : fibonacci(n-1) + fibonacci(n-2);
}


/* Generate random permutation of numbers from range <0, length). */
std::vector<int> random_permutation(int length) {
    std::vector<int> v;
    for (int i = 0; i < length; ++i) v.push_back(i);
    std::random_shuffle(v.begin(), v.end());
    return v;
}


/* Random generator suitable for smaller ranges (smaller compared to RAND_MAX macro). */
int random(int min, int max) {
    assert(min < max);

    // `std::rand` returns a pseudo-random integral value between 0 and RAND_MAX
    // (0 and RAND_MAX included).
    return min + (std::rand() % (max-min+1));
}


/* Standard IO is not synchronized by definition, so this function does it.
 * It also prepend thread ID before each call. And its printf-style! */
void sync_printf(const char *format, ...) {
    // Lock for synchronizing output.
    static std::mutex m;

    // Convert ‘std::thread::id’ to ‘__va_list_tag* is impossible, so convert it into
    // std::string.
    std::stringstream ss;
    ss << std::this_thread::get_id();
    std::string thread_id_str = ss.str();

    va_list args;
    va_start(args, format);

    {
        std::lock_guard<std::mutex> lock(m);
        printf("[%s] ", thread_id_str.c_str());
        vprintf(format, args);
    }

    va_end(args);
}


/****************************************************************************************
 * DATA STRUCTURES
 ***************************************************************************************/

struct Buffer {
    // Mutex that guards condition_variable and data.
    std::mutex mutex;
    // The condition_variable class is a synchronization primitive that can be used
    // to block a thread, or multiple threads at the same time, until another
    // thread both modifies a shared variable (the condition), and notifies the
    // condition_variable.
    std::condition_variable cond_var;

    struct BufferEntry {
        int position;
        int number;
    };

    // Producer adds new entries to the end, entries are consumed from the front.
    std::deque<BufferEntry> data;
    // When producer finish its work, set this property to true.
    bool production_is_finished = false;
};


/****************************************************************************************
 * PRODUCER-CONSUMER
 ***************************************************************************************/

void consumer(Buffer &buffer, int * results) {
    sync_printf("consumer: Starting.\n");

    bool do_run = true;

    while (do_run) {
        // Acquire lock used to protect shared data.
        std::unique_lock<std::mutex> lock(buffer.mutex);
        // `wait` causes the current thread to block until the condition
        // variable is notified or a spurious wakeup occurs.
        while (buffer.data.empty() && ! buffer.production_is_finished) {
            buffer.cond_var.wait(lock);
        }
        // While cycle can be replaced by:
        //    buffer.cond_var.wait(
        //          lock, [&buffer]{
        //              return ! buffer.data.empty() || buffer.production_is_finished;
        //          });

        // Here the lock is acquired again!

        // Handle the data.
        if (buffer.data.empty()) {
            do_run = (! buffer.production_is_finished);
            lock.unlock();
        } else {
            // Unload the front entry.
            Buffer::BufferEntry be = buffer.data.front();
            buffer.data.pop_front();

            // Lock is not needed anymore, all data are local!
            lock.unlock();

            sync_printf("consumer: Acquired data to process: %d.\n", be.number);

            // Do the work.
            int fib_result = fibonacci(be.number);
            sync_printf("consumer: Calculation result: fib(%d) = %d.\n",
                    be.number, fib_result);
            results[be.position] = fib_result;
        }
    }

    sync_printf("consumer: Ending.\n");
}


// Just to test the two approaches, producer takes buffer as a reference, permutation as a pointer.
void producer(Buffer &buffer, std::vector<int> *permutation) {
    for (auto it = permutation->begin(); it != permutation->end(); ++it) {
        // Sleep for random time, but randomly choose whether to sleep at all.
        if (random(1,CONSUMER_SLEEP_PART) != 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(random(1, MAX_SLEEP_TIME_MS)));
        }

        {
            // Even if the shared variable is atomic, it must be modified under
            // the mutex in order to correctly publish the modification to the
            // waiting thread.
            std::lock_guard<std::mutex> lock(buffer.mutex);

            buffer.data.push_back(Buffer::BufferEntry{*it, DATA_START + *it});

            sync_printf("producer: New data: %d.\n", *it);
        }
        // The lock does not need to be held for notification.
        buffer.cond_var.notify_one();
    }

    sync_printf("producer: Everything is produced. Signalling the end of production.\n");

    // End processing.
    {
        // Lock mutex `buffer.mutex`. The mutex is automatically released when `lock`
        // goes out of scope.
        // The class `lock_guard` is a mutex wrapper that provides a convenient RAII-style
        // mechanism for owning a mutex for the duration of a scoped block
        // (RAII = Resource Acquisition Is Initialization).
        std::lock_guard<std::mutex> lock(buffer.mutex);
        buffer.production_is_finished = true;
    }
    // Wake up all waiting threads so they can finish.
    buffer.cond_var.notify_all();

    sync_printf("producer: Ending.\n");
}


int main() {
    assert(DATA_LENGTH >= 2);

    Buffer buffer;
    int results[DATA_LENGTH];
    auto permutation = random_permutation(DATA_LENGTH);
    std::vector<std::thread> consumers;

    // Set seed to random generator.
    std::srand(RANDOM_GENERATOR_SEED);

    std::cout << "Starting main thread." << std::endl;

    // Start consumers.
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumers.push_back(std::thread(consumer, std::ref(buffer), results));
    }

    // Start producer.
    //producer(buffer, &permutation);  // Start in this thread.
    std::thread producerThread = std::thread(producer, std::ref(buffer), &permutation);

    // Join all threads.
    for (auto it = consumers.begin(); it != consumers.end(); ++it) {
        it->join();
    }
    producerThread.join();

    // Check results!
    std::cout << "Checking results." << std::endl;
    assert(results[0] == fibonacci(DATA_START));
    assert(results[1] == fibonacci(DATA_START+1));
    for (int i = 2; i < DATA_LENGTH; ++i) {
        assert(results[i-2] + results[i-1] == results[i]);
    }
    std::cout << "Results OK!" << std::endl;

    std::cout << "Ending main thread." << std::endl;
}

