#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <condition_variable>
#include <queue>
#include <algorithm>
#include <optional>

using Task = std::function<void()>;

class ThreadPool;

class Worker {
public:
    explicit Worker(ThreadPool& tp) : m_thread([this]{this->runKernel(); }), m_threadpool(tp){}
    [[nodiscard]] bool isDone() const { return done; }
    void requestStop() {
        m_thread.request_stop();
    }

private:
    void runKernel();
    bool done{false};
    std::jthread m_thread;
    ThreadPool& m_threadpool;
};

class Queue {
public:
    Queue() = default;

    std::optional<Task> pop(std::stop_token& st) {
        std::unique_lock lck(mtx);
        m_q_cv.wait(lck, st, [this] { return !q.empty(); });
        if(!st.stop_requested()) {
            auto task = q.front();
            q.pop_front();
            return std::move(task);
        }

        return {};
    }

    void push(Task&& t) {
        std::unique_lock lck(mtx);
        q.push_back(std::move(t));
        m_q_cv.notify_one();
    }

    void push(const Task& t) {
        std::unique_lock lck(mtx);
        q.push_back(t);
        m_q_cv.notify_one();
    }

    [[nodiscard]] std::size_t getSize() {
        std::unique_lock lck(mtx);
        return q.size();
    }

    [[nodiscard]] bool isEmpty() {
        std::unique_lock lck(mtx);
        return q.empty();
    }

private:
    std::deque<Task> q{};
    std::condition_variable_any m_q_cv{};
    std::mutex mtx;
};

class ThreadPool {
public:
    explicit ThreadPool(std::size_t sz) : q(Queue()) {
        m_thread_vec.reserve(sz);
        for (auto i = 0u; i < sz; ++i) {
            m_thread_vec.emplace_back(*this);
        }
    }

    void addTask(Task task) {
        q.push(std::move(task));
    }

    std::optional<Task> getTask(std::stop_token& st) {
        return q.pop(st);
    }

    void notifyWorkerDone() {
        std::unique_lock lock(mtx);
        m_allDone_cv.notify_one();
    }

    void waitUntilFinish() {
        std::unique_lock lock(mtx);
        m_allDone_cv.wait(lock, [this] {
            return q.isEmpty() && std::all_of(m_thread_vec.begin(), m_thread_vec.end(), [](const Worker& w) {
                return w.isDone();
            });
        });
    }


private:
    Queue q;
    std::mutex mtx;
    std::condition_variable m_allDone_cv;
    std::vector<Worker> m_thread_vec;
};

void Worker::runKernel() {
    auto st = m_thread.get_stop_token();
    while (auto task = m_threadpool.getTask(st)) {
        done = false;
        (*task)();
        done = true;
        m_threadpool.notifyWorkerDone();
    }
    done = true;
    m_threadpool.notifyWorkerDone();
}

int main() {
    std::atomic<int> i{0};
    auto sample = [&i] {++i; };
    ThreadPool tp(4);
    for (int j = 0; j < 10405; ++j) {
        tp.addTask(sample);
    }
    tp.waitUntilFinish();
    std::cout << "prlp " << i << " prpl" << std::endl;
    return 0;
}