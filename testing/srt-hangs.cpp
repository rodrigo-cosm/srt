#include <srt/srt.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class SrtClient {
public:
    SrtClient();
    ~SrtClient();

    void start_watchdog();
    int open(const std::string& address, const std::string& port);
    int send(char* data, size_t size);
    int close();

    void watchdog();
    int send_packetized(const char* data, size_t size);

private:
    int m_srtSocket { SRT_INVALID_SOCK };
    std::atomic_bool m_stopStreamer { false };

    std::atomic<std::chrono::high_resolution_clock::duration> m_sendStartTime {};
    std::condition_variable m_sendFinishedCondition;
    std::atomic_bool m_sendFinished { false };
    std::mutex m_sendFinishedMtx;
    std::thread mWatchdog;
};

SrtClient::SrtClient()
{
    std::cout << "using SRT version " << SRT_VERSION_STRING << "\n";
    if (srt_startup() == -1) {
        std::cout << "Error initializing SRT library, exit! \n";
        exit(-1);
    }

    srt_resetlogfa(NULL, 0);
}

SrtClient::~SrtClient()
{
    close();
    if (mWatchdog.joinable()) {
        mWatchdog.join();
    }
    srt_cleanup();
}

void SrtClient::start_watchdog()
{
    mWatchdog = std::thread(&SrtClient::watchdog, this);
}

int SrtClient::open(const std::string& address, const std::string& port)
{
    std::cout << "Opening SRT streamer for " << address << ":" << port << "\n";

    m_srtSocket = srt_create_socket();
    if (m_srtSocket == SRT_INVALID_SOCK) {
        std::cout << "Failed to create a SRT socket. Reason: " << srt_getlasterror_str() << "\n";
        return 1;
    }

    struct sockaddr_in sa { };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(std::stoul(port));
    if (inet_pton(AF_INET, address.c_str(), &sa.sin_addr) != 1) {
        std::cout << "Unrecognized address " << address << ":" << port << " while opening SRT socket\n";
        return 1;
    }

    if (srt_connect(m_srtSocket, (struct sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
        std::cout << "Error while connecting to " << address << ":" << port << ". Reason: " << srt_getlasterror_str() << "\n";
        return 1;
    }

    std::cout << "SRT streamer opened \n";

    return 0;
}

int SrtClient::close()
{
    if (srt_close(m_srtSocket) == SRT_ERROR) {
        std::cout << "srt_close failed: " << srt_getlasterror_str() << "\n";
    }
    m_srtSocket = SRT_INVALID_SOCK;

    return 0;
}

int SrtClient::send(char* data, size_t size)
{
    static int i = 0;
    if (i++ % 2 == 0) {
        fprintf(stderr, "Enabling logs for this send\n");
        srt_resetlogfa(NULL, 0);
        srt_addlogfa(SRT_LOGFA_QUE_SEND);
        srt_addlogfa(SRT_LOGFA_INTERNAL);
    } else {
        fprintf(stderr, "Disabling logs for this send\n");
        srt_resetlogfa(NULL, 0);
    }
    return send_packetized(data, size);
}

int SrtClient::send_packetized(const char* data, size_t size)
{
    int remaining = size;
    int sent = 0;

    if (remaining <= 0) {
        return 0;
    }

    do {
        const int chunkSize = std::min(1316, remaining);

        m_sendStartTime = std::chrono::high_resolution_clock::now().time_since_epoch();
        const int st = srt_send(m_srtSocket, data + sent, chunkSize);
        {
            std::unique_lock<std::mutex> lck(m_sendFinishedMtx);
            m_sendFinished = true;
            m_sendFinishedCondition.notify_one();
        }

        if (st != SRT_ERROR) {
            remaining -= st;
            sent += st;
        } else {
            std::cout << "srt_send failed: " << srt_getlasterror_str() << "\n";
            return -1;
        }
    } while (remaining > 0);

    return sent;
}

void SrtClient::watchdog()
{
    std::cout << "SRT watchdog started\n";

    while (!m_stopStreamer) {
        std::unique_lock<std::mutex> lck(m_sendFinishedMtx);
        bool fulfilled = m_sendFinishedCondition.wait_for(
            lck, std::chrono::seconds { 2 },
            [this] { return m_sendFinished || m_stopStreamer; });

        if (!fulfilled) {
            auto now = std::chrono::high_resolution_clock::now();
            auto before = std::chrono::high_resolution_clock::time_point { m_sendStartTime };
            std::cout << "SRT send() hangs for " << std::chrono::duration_cast<std::chrono::milliseconds>(now - before).count() << " ms" << std::endl;
        } else {
            m_sendFinished = false;
        }
    }
}

int main(int argc, char* argv[])
{
    int bitrate = 100'000'000; // bps
    if (argc > 1) {
        bitrate = std::stoi(argv[1]);
    }

    std::string addr = "12.34.56.78";
    if (argc > 2) {
        addr = argv[2];
    }
    std::string port = "6666";
    if (argc > 3) {
        port = argv[3];
    }

    SrtClient streamer;
    if (streamer.open(addr, port) != 0) {
        return -1;
    }

    streamer.start_watchdog();

    std::vector<char> payload(bitrate / 8); // bytes to send

    while (true) {
        auto before = std::chrono::high_resolution_clock::now();
        streamer.send(payload.data(), payload.size());
        auto after = std::chrono::high_resolution_clock::now();

        auto send_time = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
        std::cout << "send took " << send_time.count() << " ms \n";

        auto sleep_duration = std::chrono::milliseconds { 1000 } - send_time;
        if (sleep_duration > std::chrono::microseconds::zero())
            std::this_thread::sleep_for(std::chrono::milliseconds { 1000 } - send_time);
    }

    streamer.close();
}
