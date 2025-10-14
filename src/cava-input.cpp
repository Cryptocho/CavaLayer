#include <atomic>
#include <vector>
#include <thread>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "cava-input.hpp"

// Internal state
static std::thread reader_thread;
static std::atomic<int> running{0};
static size_t g_bars_number = CAVA_BARS_NUMBER;
static size_t g_bytes_per_sample = 2;
static float g_max_value = 65535.0f;

// ring buffer (SPSC) storing contiguous frames
static float *ring_buf = nullptr;     // allocated as (ring_capacity * bars)
static size_t ring_capacity = 0;      // number of frames
static std::atomic<size_t> head{0};   // producer index (next to write)
static std::atomic<size_t> tail{0};   // consumer index (next to read)
static size_t ring_mask = 0;          // if capacity is power of two

// child process pid and pipe fd
static pid_t child_pid = -1;
static int cava_stdout_fd = -1;
static char tmp_config_path[128] = {0};

static inline bool is_power_of_two(size_t x) { return x && ((x & (x - 1)) == 0); }

// create temp config file (mkstemp) and write config content
static int create_temp_config(const char *bit_format, size_t bars, char *out_path, size_t out_path_len) {
    char template_path[] = "/tmp/cava_cfg_XXXXXX";
    int fd = mkstemp(template_path);
    if (fd < 0) return -1;
    // keep path for later removal
    strncpy(out_path, template_path, out_path_len - 1);
    out_path[out_path_len - 1] = '\0';

    // compose config similar to Rust code
    std::string config = "[general]\n";
    config += "bars = " + std::to_string(bars) + "\n";
    config += "[output]\n";
    config += "method = raw\n";
    config += "raw_target = /dev/stdout\n";
    config += "bit_format = ";
    config += bit_format;
    config += "\n";

    ssize_t w = write(fd, config.c_str(), config.size());
    (void)w;
    fsync(fd);
    close(fd);
    return 0;
}

// spawn cava with args: cava -p <config_path>
// returns child's pid and sets out_fd to read end of stdout pipe
static pid_t spawn_cava_and_pipe_stdout(const char *config_path, int *out_fd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        // child
        // ensure child dies if parent dies
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        // move write end to stdout
        dup2(pipefd[1], STDOUT_FILENO);
        // close read end in child
        close(pipefd[0]);
        close(pipefd[1]);

        // exec cava
        const char *argv[] = {"cava", "-p", config_path, nullptr};
        execvp(argv[0], (char * const*)argv);
        // if exec fails
        _exit(127);
    } else {
        // parent: close write end, return read end
        close(pipefd[1]);
        *out_fd = pipefd[0];
        // we will do blocking reads in reader thread
        return pid;
    }
}

static void reader_thread_func() {
    // block signals in this thread if desired (keep default)
    size_t chunk_size = g_bytes_per_sample * g_bars_number;
    std::vector<uint8_t> buffer(chunk_size);
    ssize_t r;

    while (running.load(std::memory_order_acquire)) {
        // read exactly chunk_size bytes (blocking). handle short reads.
        size_t got = 0;
        while (got < chunk_size) {
            r = ::read(cava_stdout_fd, buffer.data() + got, chunk_size - got);
            if (r > 0) {
                got += (size_t)r;
            } else if (r == 0) {
                // EOF: cava exited
                running.store(0);
                break;
            } else {
                if (errno == EINTR) continue;
                // non-recoverable read error
                running.store(0);
                break;
            }
        }
        if (!running.load(std::memory_order_acquire)) break;
        // parse to floats
        // write into ring buffer non-blocking; if full, drop the new frame (mimic try_send)
        size_t cur_head = head.load(std::memory_order_relaxed);
        size_t next_head = (cur_head + 1) & ring_mask;
        size_t cur_tail = tail.load(std::memory_order_acquire);
        if (next_head == cur_tail) {
            // full -> drop frame
            continue;
        }
        float *slot_ptr = ring_buf + (cur_head * g_bars_number);
        if (g_bytes_per_sample == 2) {
            for (size_t i = 0; i < g_bars_number; ++i) {
                uint8_t lo = buffer[i*2];
                uint8_t hi = buffer[i*2 + 1];
                uint16_t v = (uint16_t)lo | ((uint16_t)hi << 8);
                slot_ptr[i] = (float)v / g_max_value;
            }
        } else {
            for (size_t i = 0; i < g_bars_number; ++i) {
                uint8_t b = buffer[i];
                slot_ptr[i] = (float)b / g_max_value;
            }
        }
        // publish by moving head
        head.store(next_head, std::memory_order_release);
    } // loop

    // cleanup: close pipe and reap child
    if (cava_stdout_fd >= 0) {
        close(cava_stdout_fd);
        cava_stdout_fd = -1;
    }
    if (child_pid > 0) {
        // try to waitpid non-blocking to avoid zombie
        int status = 0;
        waitpid(child_pid, &status, WNOHANG);
        child_pid = -1;
    }
}

// PUBLIC API
int cava_reader_start(const char *bit_format, size_t bars_number, size_t ring_capacity_in) {
    if (running.load(std::memory_order_acquire)) {
        return CAVA_ERR; // already running
    }
    if (!bit_format) return CAVA_ERR;
    g_bars_number = bars_number > 0 ? bars_number : CAVA_BARS_NUMBER;
    if (strcmp(bit_format, "16bit") == 0) {
        g_bytes_per_sample = 2;
        g_max_value = 65535.0f;
    } else if (strcmp(bit_format, "8bit") == 0) {
        g_bytes_per_sample = 1;
        g_max_value = 255.0f;
    } else {
        return CAVA_ERR;
    }

    // ring capacity: must be power of two and >= 2
    if (ring_capacity_in < 2) ring_capacity_in = 2;
    size_t cap = ring_capacity_in;
    // round up to power of two if not
    if (!is_power_of_two(cap)) {
        size_t p = 1;
        while (p < cap) p <<= 1;
        cap = p;
    }
    ring_capacity = cap;
    ring_mask = ring_capacity - 1;

    // allocate ring buffer
    // free old if existed
    if (ring_buf) {
        free(ring_buf);
        ring_buf = nullptr;
    }
    ring_buf = (float*)malloc(sizeof(float) * ring_capacity * g_bars_number);
    if (!ring_buf) return CAVA_ERR;
    memset(ring_buf, 0, sizeof(float) * ring_capacity * g_bars_number);
    head.store(0);
    tail.store(0);

    // create temp config
    if (create_temp_config(bit_format, g_bars_number, tmp_config_path, sizeof(tmp_config_path)) != 0) {
        free(ring_buf);
        ring_buf = nullptr;
        return CAVA_ERR;
    }

    // spawn cava
    int out_fd = -1;
    pid_t pid = spawn_cava_and_pipe_stdout(tmp_config_path, &out_fd);
    if (pid <= 0) {
        unlink(tmp_config_path);
        free(ring_buf);
        ring_buf = nullptr;
        return CAVA_ERR;
    }
    child_pid = pid;
    cava_stdout_fd = out_fd;

    running.store(1);
    // start thread
    reader_thread = std::thread(reader_thread_func);
    return CAVA_OK;
}

void cava_reader_stop(void) {
    if (!running.load(std::memory_order_acquire)) return;
    running.store(0);
    // close read fd to wake thread
    if (cava_stdout_fd >= 0) {
        close(cava_stdout_fd);
        cava_stdout_fd = -1;
    }
    // kill child if still alive
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        // wait for child
        int status = 0;
        waitpid(child_pid, &status, 0);
        child_pid = -1;
    }
    if (reader_thread.joinable()) reader_thread.join();
    // cleanup config file
    if (tmp_config_path[0] != '\0') {
        unlink(tmp_config_path);
        tmp_config_path[0] = '\0';
    }
    // free buffer
    if (ring_buf) {
        free(ring_buf);
        ring_buf = nullptr;
    }
    ring_capacity = 0;
    ring_mask = 0;
    head.store(0);
    tail.store(0);
}

int cava_reader_try_pop(float *out_buf, size_t max_len) {
    if (!out_buf) return -1;
    if (!running.load(std::memory_order_acquire) && head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire)) {
        return 0;
    }
    if (max_len < g_bars_number) return -1;
    size_t cur_tail = tail.load(std::memory_order_relaxed);
    size_t cur_head = head.load(std::memory_order_acquire);
    if (cur_tail == cur_head) {
        return 0; // empty
    }
    float *slot_ptr = ring_buf + (cur_tail * g_bars_number);
    memcpy(out_buf, slot_ptr, sizeof(float) * g_bars_number);
    // advance tail
    tail.store((cur_tail + 1) & ring_mask, std::memory_order_release);
    return 1;
}

size_t cava_reader_bars_number(void) {
    return g_bars_number;
}

int cava_reader_running(void) {
    return running.load(std::memory_order_acquire) ? 1 : 0;
}