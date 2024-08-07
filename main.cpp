#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include <condition_variable>
#include <queue>
#include <algorithm>
#include <future>
#include <optional>

using Task = std::move_only_function<void()>;

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
            auto task = std::move(q.front());
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
    explicit ThreadPool(std::size_t sz) {
        m_thread_vec.reserve(sz);
        for (auto i = 0u; i < sz; ++i) {
            m_thread_vec.emplace_back(*this);
        }
    }

    template<typename Func, typename... Args>
    auto addTask(Func&& f, Args&&... args) {
        using ReturnType = std::invoke_result_t<Func, Args...>;
        std::packaged_task<ReturnType()> pt =  std::packaged_task<ReturnType()>{ std::bind(
                std::forward<Func>(f), std::forward<Args>(args)...)
        };
        auto ftr = pt.get_future();
        Task task {[pt = std::move(pt)] mutable {pt();}};
        q.push(std::move(task));
        return ftr;
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

namespace task {

    template<typename Func, typename... Args>
    auto createTask2(Func&& f, Args&&... args) {
        using ReturnType = std::invoke_result_t<Func, Args...>;
        return std::packaged_task<ReturnType()>{ std::bind(
                std::forward<Func>(f), std::forward<Args>(args)...)
        };
    }
}
int main() {

    auto t1 = [](int i){return i+1;};
    auto t2 = [](int i,int j)->int{return i+j;};

    ThreadPool tp(4);
    std::vector<std::future<int>> futures;
    for(int i=0;i<32;++i) {
        futures.push_back(tp.addTask(t1,rand()%20));
        futures.push_back(tp.addTask(t2,rand()%20,rand()%42));
    }
    for(int i=0;i<futures.size();++i) {
        std::cout << futures[i].get() << "\n";
    }
    return 0;
}