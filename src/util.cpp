#include "util.h"
#include <filesystem>
#include <sstream>
#include <physfs.h>
#include <spdlog/spdlog.h>
#include "xml.h"

thread_local static int s_current_thread_id = 0;

namespace twogame::util {

ThreadPool::Thread::Thread(int thread_id)
    : m_thread(&ThreadPool::Thread::work, this)
    , m_thread_id(thread_id)
    , m_destroying(false)
{
}

ThreadPool::Thread::~Thread()
{
    if (m_thread.joinable()) {
        wait_idle();
        {
            std::scoped_lock lk(m_mtx_tasks);
            m_destroying = true;
            m_cv_tasks.notify_one();
        }
        m_thread.join();
    }
}

void ThreadPool::Thread::wait_idle()
{
    std::unique_lock lk(m_mtx_tasks);
    m_cv_tasks.wait(lk, [this]() { return m_tasks.empty(); });
}

void ThreadPool::Thread::work()
{
    s_current_thread_id = m_thread_id;
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(m_mtx_tasks);
            m_cv_tasks.wait(lk, [this]() { return m_tasks.empty() == false || m_destroying; });
            if (m_destroying)
                break;
            job = m_tasks.front();
        }
        job();
        {
            std::scoped_lock lk(m_mtx_tasks);
            m_tasks.pop();
            m_cv_tasks.notify_one();
        }
    }
}

ThreadPool::ThreadPool(size_t thread_count)
{
    int concurrency = static_cast<int>(std::thread::hardware_concurrency()) - 1;
    if (thread_count == 0)
        m_threads.resize(std::max(1, concurrency));
    else
        m_threads.resize(std::min(thread_count, static_cast<size_t>(std::max(1, concurrency))));

    spdlog::info("thread pool: created with {} threads", m_threads.size());
    for (size_t i = 0; i < m_threads.size(); i++) {
        m_threads[i] = std::make_unique<ThreadPool::Thread>(i + 1);
    }
}

void ThreadPool::wait_idle()
{
    for (auto& thread : m_threads)
        thread->wait_idle();
}

int ThreadPool::current_thread_id()
{
    return s_current_thread_id;
}

std::string resolve_path(std::string_view current, std::string_view relative)
{
    if (relative.front() == '/')
        return std::string { relative };

    auto p = std::filesystem::path(current).parent_path();
    auto append = [&p](const std::string_view& el) {
        if (el == "..")
            p = p.parent_path();
        else if (el != ".")
            p /= el;
    };

    std::string_view::size_type start = 0, end;
    while ((end = relative.find_first_of("/")) != std::string_view::npos) {
        append(relative.substr(start, end - start));
        start = relative.find_first_not_of("/", end + 1);
    }
    if (start != std::string_view::npos) {
        append(relative.substr(start));
    }

    return p.generic_string();
}

}

namespace twogame::xml {

Exception::Exception(const pugi::xml_node& node, std::string_view prop)
{
    std::ostringstream oss;
    oss << node.path() << ": bad '" << prop << "'";
    m_what = oss.str();
}

}

namespace twogame::xml::priv {

bool slurp(const std::string& path, pugi::xml_document& doc)
{
    PHYSFS_Stat stat;
    PHYSFS_File* fh;

    if (PHYSFS_stat(path.c_str(), &stat) == 0)
        return false;
    if ((fh = PHYSFS_openRead(path.c_str())) == nullptr)
        return false;

    void* buffer = pugi::get_memory_allocation_function()(stat.filesize);
    if (PHYSFS_readBytes(fh, buffer, stat.filesize) < stat.filesize) {
        pugi::get_memory_deallocation_function()(buffer);
        PHYSFS_close(fh);
        return false;
    }

    return doc.load_buffer_inplace_own(buffer, stat.filesize);
}

bool parse_boolean(const std::string_view& s)
{
    return s == "true" || s == "yes";
}

}
